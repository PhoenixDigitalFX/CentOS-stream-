// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 */

#include <hyp/switch.h>
#include <hyp/sysreg-sr.h>

#include <linux/arm-smccc.h>
#include <linux/kvm_host.h>
#include <linux/types.h>
#include <linux/jump_label.h>
#include <uapi/linux/psci.h>

#include <kvm/arm_psci.h>

#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/kprobes.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/fpsimd.h>
#include <asm/debug-monitors.h>
#include <asm/processor.h>
#include <asm/thread_info.h>

static void __activate_traps(struct kvm_vcpu *vcpu)
{
	u64 val;

	___activate_traps(vcpu);
	__activate_traps_common(vcpu);

	val = CPTR_EL2_DEFAULT;
	val |= CPTR_EL2_TTA | CPTR_EL2_TZ | CPTR_EL2_TAM;
	if (!update_fp_enabled(vcpu)) {
		val |= CPTR_EL2_TFP;
		__activate_traps_fpsimd32(vcpu);
	}

	write_sysreg(val, cptr_el2);

	if (cpus_have_final_cap(ARM64_WORKAROUND_SPECULATIVE_AT)) {
		struct kvm_cpu_context *ctxt = &vcpu->arch.ctxt;

		isb();
		/*
		 * At this stage, and thanks to the above isb(), S2 is
		 * configured and enabled. We can now restore the guest's S1
		 * configuration: SCTLR, and only then TCR.
		 */
		write_sysreg_el1(ctxt_sys_reg(ctxt, SCTLR_EL1),	SYS_SCTLR);
		isb();
		write_sysreg_el1(ctxt_sys_reg(ctxt, TCR_EL1),	SYS_TCR);
	}
}

static void __deactivate_traps(struct kvm_vcpu *vcpu)
{
	u64 mdcr_el2;

	___deactivate_traps(vcpu);

	mdcr_el2 = read_sysreg(mdcr_el2);

	if (cpus_have_final_cap(ARM64_WORKAROUND_SPECULATIVE_AT)) {
		u64 val;

		/*
		 * Set the TCR and SCTLR registers in the exact opposite
		 * sequence as __activate_traps (first prevent walks,
		 * then force the MMU on). A generous sprinkling of isb()
		 * ensure that things happen in this exact order.
		 */
		val = read_sysreg_el1(SYS_TCR);
		write_sysreg_el1(val | TCR_EPD1_MASK | TCR_EPD0_MASK, SYS_TCR);
		isb();
		val = read_sysreg_el1(SYS_SCTLR);
		write_sysreg_el1(val | SCTLR_ELx_M, SYS_SCTLR);
		isb();
	}

	__deactivate_traps_common();

	mdcr_el2 &= MDCR_EL2_HPMN_MASK;
	mdcr_el2 |= MDCR_EL2_E2PB_MASK << MDCR_EL2_E2PB_SHIFT;

	write_sysreg(mdcr_el2, mdcr_el2);
	write_sysreg(HCR_HOST_NVHE_FLAGS, hcr_el2);
	write_sysreg(CPTR_EL2_DEFAULT, cptr_el2);
}

static void __deactivate_vm(struct kvm_vcpu *vcpu)
{
	write_sysreg(0, vttbr_el2);
}

/* Save VGICv3 state on non-VHE systems */
static void __hyp_vgic_save_state(struct kvm_vcpu *vcpu)
{
	if (static_branch_unlikely(&kvm_vgic_global_state.gicv3_cpuif)) {
		__vgic_v3_save_state(&vcpu->arch.vgic_cpu.vgic_v3);
		__vgic_v3_deactivate_traps(&vcpu->arch.vgic_cpu.vgic_v3);
	}
}

/* Restore VGICv3 state on non_VEH systems */
static void __hyp_vgic_restore_state(struct kvm_vcpu *vcpu)
{
	if (static_branch_unlikely(&kvm_vgic_global_state.gicv3_cpuif)) {
		__vgic_v3_activate_traps(&vcpu->arch.vgic_cpu.vgic_v3);
		__vgic_v3_restore_state(&vcpu->arch.vgic_cpu.vgic_v3);
	}
}

/**
 * Disable host events, enable guest events
 */
static bool __pmu_switch_to_guest(struct kvm_cpu_context *host_ctxt)
{
	struct kvm_host_data *host;
	struct kvm_pmu_events *pmu;

	host = container_of(host_ctxt, struct kvm_host_data, host_ctxt);
	pmu = &host->pmu_events;

	if (pmu->events_host)
		write_sysreg(pmu->events_host, pmcntenclr_el0);

	if (pmu->events_guest)
		write_sysreg(pmu->events_guest, pmcntenset_el0);

	return (pmu->events_host || pmu->events_guest);
}

/**
 * Disable guest events, enable host events
 */
static void __pmu_switch_to_host(struct kvm_cpu_context *host_ctxt)
{
	struct kvm_host_data *host;
	struct kvm_pmu_events *pmu;

	host = container_of(host_ctxt, struct kvm_host_data, host_ctxt);
	pmu = &host->pmu_events;

	if (pmu->events_guest)
		write_sysreg(pmu->events_guest, pmcntenclr_el0);

	if (pmu->events_host)
		write_sysreg(pmu->events_host, pmcntenset_el0);
}

/* Switch to the guest for legacy non-VHE systems */
int __kvm_vcpu_run(struct kvm_vcpu *vcpu)
{
	struct kvm_cpu_context *host_ctxt;
	struct kvm_cpu_context *guest_ctxt;
	bool pmu_switch_needed;
	u64 exit_code;

	/*
	 * Having IRQs masked via PMR when entering the guest means the GIC
	 * will not signal the CPU of interrupts of lower priority, and the
	 * only way to get out will be via guest exceptions.
	 * Naturally, we want to avoid this.
	 */
	if (system_uses_irq_prio_masking()) {
		gic_write_pmr(GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET);
		pmr_sync();
	}

	vcpu = kern_hyp_va(vcpu);

	host_ctxt = &__hyp_this_cpu_ptr(kvm_host_data)->host_ctxt;
	host_ctxt->__hyp_running_vcpu = vcpu;
	guest_ctxt = &vcpu->arch.ctxt;

	pmu_switch_needed = __pmu_switch_to_guest(host_ctxt);

	__sysreg_save_state_nvhe(host_ctxt);

	/*
	 * We must restore the 32-bit state before the sysregs, thanks
	 * to erratum #852523 (Cortex-A57) or #853709 (Cortex-A72).
	 *
	 * Also, and in order to be able to deal with erratum #1319537 (A57)
	 * and #1319367 (A72), we must ensure that all VM-related sysreg are
	 * restored before we enable S2 translation.
	 */
	__sysreg32_restore_state(vcpu);
	__sysreg_restore_state_nvhe(guest_ctxt);

	__activate_vm(kern_hyp_va(vcpu->kvm));
	__activate_traps(vcpu);

	__hyp_vgic_restore_state(vcpu);
	__timer_enable_traps(vcpu);

	__debug_switch_to_guest(vcpu);

	__set_guest_arch_workaround_state(vcpu);

	do {
		/* Jump in the fire! */
		exit_code = __guest_enter(vcpu, host_ctxt);

		/* And we're baaack! */
	} while (fixup_guest_exit(vcpu, &exit_code));

	__set_host_arch_workaround_state(vcpu);

	__sysreg_save_state_nvhe(guest_ctxt);
	__sysreg32_save_state(vcpu);
	__timer_disable_traps(vcpu);
	__hyp_vgic_save_state(vcpu);

	__deactivate_traps(vcpu);
	__deactivate_vm(vcpu);

	__sysreg_restore_state_nvhe(host_ctxt);

	if (vcpu->arch.flags & KVM_ARM64_FP_ENABLED)
		__fpsimd_save_fpexc32(vcpu);

	/*
	 * This must come after restoring the host sysregs, since a non-VHE
	 * system may enable SPE here and make use of the TTBRs.
	 */
	__debug_switch_to_host(vcpu);

	if (pmu_switch_needed)
		__pmu_switch_to_host(host_ctxt);

	/* Returning to host will clear PSR.I, remask PMR if needed */
	if (system_uses_irq_prio_masking())
		gic_write_pmr(GIC_PRIO_IRQOFF);

	return exit_code;
}

void __noreturn hyp_panic(struct kvm_cpu_context *host_ctxt)
{
	u64 spsr = read_sysreg_el2(SYS_SPSR);
	u64 elr = read_sysreg_el2(SYS_ELR);
	u64 par = read_sysreg(par_el1);
	struct kvm_vcpu *vcpu = host_ctxt->__hyp_running_vcpu;
	unsigned long str_va;

	if (read_sysreg(vttbr_el2)) {
		__timer_disable_traps(vcpu);
		__deactivate_traps(vcpu);
		__deactivate_vm(vcpu);
		__sysreg_restore_state_nvhe(host_ctxt);
	}

	/*
	 * Force the panic string to be loaded from the literal pool,
	 * making sure it is a kernel address and not a PC-relative
	 * reference.
	 */
	asm volatile("ldr %0, =%1" : "=r" (str_va) : "S" (__hyp_panic_string));

	__hyp_do_panic(str_va,
		       spsr, elr,
		       read_sysreg(esr_el2), read_sysreg_el2(SYS_FAR),
		       read_sysreg(hpfar_el2), par, vcpu);
	unreachable();
}

asmlinkage void kvm_unexpected_el2_exception(void)
{
	return __kvm_unexpected_el2_exception();
}