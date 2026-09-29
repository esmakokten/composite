#include <vmrt.h>
#include <capmgr.h>
#include <vmx_msr.h>
#include <vm_host_msr.h>

/*
 * Hand the kernel this vCPU's value for a register the exit path no longer
 * swaps (CAPTBL_OP_VM_VCPU_MSR_SET). The kernel validates it; a refusal is
 * logged and the write is dropped, as this VMM cannot inject #GP.
 */
static void
vcpu_msr_set(struct vmrt_vm_vcpu *vcpu, u32_t sel, u64_t val, const char *name)
{
	/*
	 * Through the capability manager: the operation is invoked on a
	 * capability table, and this component holds no capability to one.
	 */
	if (capmgr_vm_vcpu_msr_set(vcpu->tid, sel, val)) {
		printc("[vmm] %s write %llx refused by the kernel\n", name, (unsigned long long)val);
	}
}

void 
rdmsr_handler(struct vmrt_vm_vcpu *vcpu)
{
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;

	switch (regs->cx) {
	case MSR_IA32_CSTAR:
	{
		/* Served from the shadow; hardware holds the host's, which the guest never sees. */
		regs->ax = (u32_t)vcpu->cstar_shadow;
		regs->dx = (u32_t)(vcpu->cstar_shadow >> 32);
		goto done;
	}
	case MSR_IA32_FMASK:
	{
		regs->ax = (u32_t)vcpu->fmask_shadow;
		regs->dx = (u32_t)(vcpu->fmask_shadow >> 32);
		goto done;
	}
	case MSR_IA32_EFER: 
	{
		u64_t guest_efer = regs->efer;
		regs->ax = (u32_t)guest_efer;
		regs->dx = (u32_t)(guest_efer >> 32);
		goto done;
	}
	case MSR_IA32_MISC_ENABLE: 
	{
		/* Note: read only MSR, and its value can be fixed for all cores */
		u64_t misc = 0x800001;
		regs->ax = (u32_t)misc;
		regs->dx = (u32_t)(misc >> 32);
		goto done;
	}
	case MSR_IA32_BIOS_SIGN_ID:
	{
		/* Note: microcode is pre-set in the kernel */
		u64_t v = regs->microcode_version;
		regs->ax = (u32_t)v;
		regs->dx = (u32_t)(v >> 32);
		goto done;
	}
	case MSR_IA32_TSC_ADJUST:
	{
		/* TODO: need to handle tsc adjustment in VM */
		u32_t ax, dx;
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}

	/* MTRRs can be ingored in Linux and Linux knows it is a virtual environment */
	case MSR_IA32_MTRR_CAP:
	case MSR_IA32_MTRR_DEF_TYPE:
	case MSR_IA32_MTRR_PHYSBASE_0:
	case MSR_IA32_MTRR_PHYSMASK_0:
	case MSR_IA32_MTRR_PHYSBASE_1:
	case MSR_IA32_MTRR_PHYSMASK_1:
	case MSR_IA32_MTRR_PHYSBASE_2:
	case MSR_IA32_MTRR_PHYSMASK_2:	
	case MSR_IA32_MTRR_PHYSBASE_3:
	case MSR_IA32_MTRR_PHYSMASK_3:
	case MSR_IA32_MTRR_PHYSBASE_4:
	case MSR_IA32_MTRR_PHYSMASK_4:
	case MSR_IA32_MTRR_PHYSBASE_5:
	case MSR_IA32_MTRR_PHYSMASK_5:
	case MSR_IA32_MTRR_PHYSBASE_6:
	case MSR_IA32_MTRR_PHYSMASK_6:
	case MSR_IA32_MTRR_PHYSBASE_7:
	case MSR_IA32_MTRR_PHYSMASK_7:
	case MSR_IA32_MTRR_PHYSBASE_8:
	case MSR_IA32_MTRR_PHYSMASK_8:
	case MSR_IA32_MTRR_PHYSBASE_9:
	case MSR_IA32_MTRR_PHYSMASK_9:	
	case MSR_IA32_MTRR_FIX64K_00000:
	case MSR_IA32_MTRR_FIX16K_80000:
	case MSR_IA32_MTRR_FIX16K_A0000:
	case MSR_IA32_MTRR_FIX4K_C0000:
	case MSR_IA32_MTRR_FIX4K_C8000:
	case MSR_IA32_MTRR_FIX4K_D0000:
	case MSR_IA32_MTRR_FIX4K_D8000:
	case MSR_IA32_MTRR_FIX4K_E0000:
	case MSR_IA32_MTRR_FIX4K_E8000:
	case MSR_IA32_MTRR_FIX4K_F0000:
	case MSR_IA32_MTRR_FIX4K_F8000:
	case MSR_IA32_PAT:
	{
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}	
	case MSR_PPERF:
	{
		/* FIXME: should inject #GP to guest, thus guest knows this MSR is not present */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}
	case MSR_SMI_COUNT:
	{
		/* FIXME: should inject #GP to guest, thus guest knows this MSR is not present */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}	
	case MSR_IA32_APIC_BASE:
	{
		/* TODO: for non-BSP, the BSP flag should be cleared */
		/* 0x900 : enable xapic and this is BSP */
		regs->ax = 0XFEE00000 | 0x900;
		regs->dx = 0;
		goto done;
	}
	case MSR_IA32_FEATURE_CONTROL:
	{
		/* Lock the feature control in VM */
		regs->ax = 1;
		regs->dx = 0;
		goto done;
	}	
	case MSR_MISC_FEATURE_ENABLES:
	{
		/* This MSR can be reserved to 0 */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}
	case MSR_PLATFORM_INFO:
	{
		/* Contains power management stuff and can be ignored */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}
	case MSR_IA32_SPEC_CTRL:
	{
		/* This MSR has a value of 0 after reset, thus just keep it ad default, don't modify */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}
	case MSR_IA32_TME_ACTIVATE:
	{
		/* TME not supported in VM */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}
	case MSR_IA32_TSC_DEADLINE:
	{
		/*
		 * The deadline the guest last armed, as the write handler recorded
		 * it; 0 once it has fired or was never set. A guest that saves and
		 * restores its own deadline around a critical section reads this.
		 */
		u64_t deadline = vcpu->next_timer == ~0ULL ? 0 : vcpu->next_timer;
		regs->ax = (u32_t)deadline;
		regs->dx = (u32_t)(deadline >> 32);
		goto done;
	}
	case MSR_PPIN_CTL:
	{
		/*
		 * Protected Processor Inventory Number control. Linux >= 6.x probes
		 * it with rdmsrl_safe() during early boot (arch/x86/kernel/cpu/intel.c)
		 * and would absorb a #GP, but this VMM has no exception-injection
		 * path, so the probe must succeed. Reading 0 means "PPIN locked and
		 * disabled", which makes the guest leave the feature alone.
		 */
		regs->ax = 0;
		regs->dx = 0;
		goto done;
	}
	default:
		VM_PANIC(vcpu);
	}
done:
	GOTO_NEXT_INST(regs);
	return;
}

void 
wrmsr_handler(struct vmrt_vm_vcpu *vcpu)
{
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;

	switch (regs->cx) {
	case MSR_IA32_CSTAR:
	{
		/*
		 * Absorbed into the shadow, never loaded: CSTAR is the SYSCALL target
		 * from compatibility mode, which Intel parts do not use (the SDM lists
		 * it, AMD parts consume it). Linux writes ignore_sysret's address once
		 * per CPU. Correct virtualization is the whole benefit: a read gives
		 * back what was written.
		 */
		vcpu->cstar_shadow = (regs->ax & 0xFFFFFFFF) | ((regs->dx & 0xFFFFFFFF) << 32);
		goto done;
	}
	case MSR_IA32_FMASK:
	{
		/*
		 * The host runs with Linux's mask (COS_HOST_FMASK), so a Linux guest's
		 * write coincides and the register is left alone at every exit. A
		 * guest that writes anything else cannot be given the register: until
		 * the fallback swap exists (rung R16) that guest is stopped here.
		 */
		u64_t val = (regs->ax & 0xFFFFFFFF) | ((regs->dx & 0xFFFFFFFF) << 32);

		vcpu->fmask_shadow = val;
		if (val != COS_HOST_FMASK) {
			printc("[vmm] FMASK write %llx differs from the host's %llx: no fallback yet\n",
			       (unsigned long long)val, (unsigned long long)COS_HOST_FMASK);
			VM_PANIC(vcpu);
		}
		goto done;
	}
	case MSR_IA32_EFER:
	{
		u64_t guest_efer;
		guest_efer = regs->ax & 0XFFFFFFFF;
		guest_efer |= ((regs->dx & 0XFFFFFFFF) << 32);
		regs->efer = guest_efer;

		goto done;
	}
	case MSR_IA32_BIOS_SIGN_ID:
	{
		/* Microcode has been retrived in the kernel, thus don't need to process here */
		goto done;
	}
	case MSR_IA32_XSS:
	{
		/* Just ignore writes to this MSR, since we don't support any of the XSAVE features represented by this MSR */
		goto done;
	}		
	case MSR_IA32_MTRR_CAP:
	case MSR_IA32_MTRR_DEF_TYPE:
	case MSR_IA32_MTRR_PHYSBASE_0:
	case MSR_IA32_MTRR_PHYSMASK_0:
	case MSR_IA32_MTRR_PHYSBASE_1:
	case MSR_IA32_MTRR_PHYSMASK_1:
	case MSR_IA32_MTRR_PHYSBASE_2:
	case MSR_IA32_MTRR_PHYSMASK_2:
	case MSR_IA32_MTRR_PHYSBASE_3:
	case MSR_IA32_MTRR_PHYSMASK_3:
	case MSR_IA32_MTRR_PHYSBASE_4:
	case MSR_IA32_MTRR_PHYSMASK_4:
	case MSR_IA32_MTRR_PHYSBASE_5:
	case MSR_IA32_MTRR_PHYSMASK_5:
	case MSR_IA32_MTRR_PHYSBASE_6:
	case MSR_IA32_MTRR_PHYSMASK_6:
	case MSR_IA32_MTRR_PHYSBASE_7:
	case MSR_IA32_MTRR_PHYSMASK_7:
	case MSR_IA32_MTRR_PHYSBASE_8:
	case MSR_IA32_MTRR_PHYSMASK_8:
	case MSR_IA32_MTRR_PHYSBASE_9:
	case MSR_IA32_MTRR_PHYSMASK_9:
	case MSR_IA32_MTRR_FIX64K_00000:
	case MSR_IA32_MTRR_FIX16K_80000:
	case MSR_IA32_MTRR_FIX16K_A0000:
	case MSR_IA32_MTRR_FIX4K_C0000:
	case MSR_IA32_MTRR_FIX4K_C8000:
	case MSR_IA32_MTRR_FIX4K_D0000:
	case MSR_IA32_MTRR_FIX4K_D8000:
	case MSR_IA32_MTRR_FIX4K_E0000:
	case MSR_IA32_MTRR_FIX4K_E8000:
	case MSR_IA32_MTRR_FIX4K_F0000:
	case MSR_IA32_MTRR_FIX4K_F8000:
	case MSR_IA32_PAT:
	{
		/* MTRRs can be ingored in Linux and Linux knows it is a virtual environment */
		goto done;
	}	
	case MSR_IA32_TSC_AUX:
	{
		/*
		 * Intercepted so the kernel always knows the loaded value: the guest's
		 * value stays in the register across exits and the kernel writes it
		 * only when a different vCPU takes the core. Linux writes it once per
		 * CPU (its cpu_number) at boot. Reads pass through.
		 */
		vcpu_msr_set(vcpu, VM_VCPU_MSR_TSC_AUX, (regs->ax & 0xFFFFFFFF) | ((regs->dx & 0xFFFFFFFF) << 32), "TSC_AUX");
		goto done;
	}
	case MSR_IA32_TSC_DEADLINE: {
		u64_t tsc_future, curr_tsc;
		rdtscll(curr_tsc);
		tsc_future = regs->ax & 0xffffffff;
		tsc_future |= ((regs->dx & 0xffffffff) << 32);
		vcpu->next_timer = tsc_future;

		goto done;
	}
	case MSR_MISC_FEATURE_ENABLES:
	{
		/* Write to this MSR will be all 0 since we ignored this, doesn't matter */
		goto done;
	}
	case MSR_IA32_SYSENTER_CS:
	case MSR_IA32_SYSENTER_ESP:
	case MSR_IA32_SYSENTER_EIP:
	{
		/* 64-bit system doesn't require these MSRs, its fine to ignore them */
		goto done;
	}
	case MSR_IA32_SPEC_CTRL:
	{
		/* This MSR has a value of 0 after reset, thus just keep it ad default, don't modify */
		goto done;
	}
	case MSR_IA32_APIC_BASE:
	{
		VM_PANIC(vcpu);
		goto done;
	}
	case MSR_PPIN_CTL:
	{
		/* The guest tries to lock PPIN after reading the control as 0; there is nothing to lock. */
		goto done;
	}
	default:
		VM_PANIC(vcpu);;
	}

done:
	GOTO_NEXT_INST(regs);
	return;
}
