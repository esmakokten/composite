#include <vmrt.h>
#include <capmgr.h>
#include <vmx_msr.h>
#include <vm_host_msr.h>

/*
 * ---------------------------------------------------------------------------
 * IA32_LSTAR interception and the guest SYSCALL trampoline.
 *
 * IA32_LSTAR holds COS_SYSCALL_TRAMP_VA on both sides of a VM crossing, so
 * the exit path never touches it. For the guest's own SYSCALL to still reach
 * entry_SYSCALL_64:
 *
 *   - the guest's LSTAR write is intercepted and absorbed into a per-vCPU
 *     shadow; hardware is never touched (a guest read returns the shadow);
 *   - at that same intercept the VMM builds a one-page trampoline in the
 *     guest at COS_SYSCALL_TRAMP_VA holding an indirect jump to the value
 *     the guest just tried to write. The intercepted value IS how the VMM
 *     learns the target: no build-time agreement, the guest binary untouched.
 *
 * Nothing is shared with the host but the number: different page tables,
 * different physical frames. The frame is mapped execute-only in EPT
 * (platform chal_pgtbl.c), so the guest can neither read nor rewrite it.
 *
 * Frames come from guest-physical memory the VMM maps but never told the
 * guest about (see COS_GUEST_TRAMP_GPA in the kernel's chal_pgtbl.h): this
 * intercept fires from syscall_init(), before the guest has a page
 * allocator, so every frame must come from here.
 *
 * Page-table isolation (PTI) is deferred: SYSCALL does not switch CR3, so on
 * a PTI guest the first instruction after it runs on the USER page tables and
 * the trampoline would also have to be installed in the user half of the PGD
 * pair; pti_init() clones only cpu_entry_area, .entry.text, espfix and
 * vsyscall, not index 509. A composition declares a PTI guest with
 * VM_GUEST_PTI=1; such a guest keeps its own LSTAR through the kernel's
 * fallback swap (rung R16) instead of the trampoline.
 * ---------------------------------------------------------------------------
 */
#include <string.h>

#define COS_SYSCALL_TRAMP_VA  0xfffffe8000000000ULL
#define TRAMP_PGD_IDX         509     /* (VA >> 39) & 511 */

#define TRAMP_GPA_PAGE  0x04000000ULL /* == COS_GUEST_TRAMP_GPA; read + execute in EPT, not writable */
#define TRAMP_GPA_PUD   0x04001000ULL
#define TRAMP_GPA_PMD   0x04002000ULL
#define TRAMP_GPA_PT    0x04003000ULL

#define PTE_P  (1ULL << 0)
#define PTE_W  (1ULL << 1)
#define PTE_A  (1ULL << 5)
#define PTE_D  (1ULL << 6)
#define PTE_G  (1ULL << 8)	/* _PAGE_GLOBAL */

#define CR3_ADDR_MASK 0x000ffffffffff000ULL

#ifndef VM_GUEST_PTI
#define VM_GUEST_PTI 0
#endif

static int
lstar_canonical(u64_t v)
{
	/* 48-bit canonical: bits 63:47 must all match. */
	u64_t hi = v >> 47;

	return (hi == 0) || (hi == 0x1ffffULL);
}

static void
guest_tramp_install(struct vmrt_vm_vcpu *vcpu, u64_t target)
{
	struct vmrt_vm_comp *vm = vcpu->vm;
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;
	u8_t  *pg  = (u8_t *)GPA2HVA(TRAMP_GPA_PAGE, vm);
	u64_t *pt  = (u64_t *)GPA2HVA(TRAMP_GPA_PT, vm);
	u64_t *pmd = (u64_t *)GPA2HVA(TRAMP_GPA_PMD, vm);
	u64_t *pud = (u64_t *)GPA2HVA(TRAMP_GPA_PUD, vm);
	u64_t *pgd = (u64_t *)GPA2HVA(regs->cr3 & CR3_ADDR_MASK, vm);
	int    i;

	/*
	 * jmp *0x0(%rip) ; .quad target
	 * Register-preserving: SYSCALL arrives with the user register file live
	 * and RAX holding the syscall number, so a movabs into a register would
	 * destroy it. The displacement is 0 because RIP already points at the
	 * address slot.
	 */
	memset(pg, 0, PAGE_SIZE_4K);
	pg[0] = 0xff;
	pg[1] = 0x25;
	pg[2] = 0x00;
	pg[3] = 0x00;
	pg[4] = 0x00;
	pg[5] = 0x00;
	*(u64_t *)&pg[6] = target;

	for (i = 0; i < (int)(PAGE_SIZE_4K / sizeof(u64_t)); i++) {
		pt[i] = pmd[i] = pud[i] = 0;
	}

	/*
	 * Supervisor-only and not writable; SYSCALL raises CPL to 0 before the
	 * fetch. Global, as Linux marks the pages it shares between its kernel
	 * and user PGDs (mm/pti.c): this page is that class.
	 */
	pt[0]  = TRAMP_GPA_PAGE | PTE_P | PTE_G | PTE_A;
	pmd[0] = TRAMP_GPA_PT   | PTE_P | PTE_W | PTE_A | PTE_D;
	pud[0] = TRAMP_GPA_PMD  | PTE_P | PTE_W | PTE_A | PTE_D;

	/* Linux's _KERNPG_TABLE, so a guest walker that checks pgd_bad() leaves it alone. */
	pgd[TRAMP_PGD_IDX] = TRAMP_GPA_PUD | PTE_P | PTE_W | PTE_A | PTE_D;

	/*
	 * Deliberately not written: the PTI user-half entry,
	 *     pgd[512 + TRAMP_PGD_IDX] = pgd[TRAMP_PGD_IDX];
	 * On a non-PTI guest there is no second page in the PGD allocation and
	 * the write would land in unrelated memory. A PTI guest is declared and
	 * takes the fallback instead (see VM_GUEST_PTI above).
	 */

	printc("[vmm] LSTAR intercept: guest target %llx -> trampoline at %llx "
	       "(gpa %llx, pgd[%d], kernel half only)\n",
	       (unsigned long long)target, (unsigned long long)COS_SYSCALL_TRAMP_VA,
	       (unsigned long long)TRAMP_GPA_PAGE, TRAMP_PGD_IDX);
}

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
	case MSR_IA32_STAR:
	{
		regs->ax = (u32_t)vcpu->star_shadow;
		regs->dx = (u32_t)(vcpu->star_shadow >> 32);
		goto done;
	}
	case MSR_IA32_LSTAR:
	{
		/* The guest's own value, never the common trampoline VA. */
		regs->ax = (u32_t)vcpu->lstar_shadow;
		regs->dx = (u32_t)(vcpu->lstar_shadow >> 32);
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
	case MSR_IA32_LSTAR:
	{
		u64_t val = (regs->ax & 0xFFFFFFFF) | ((regs->dx & 0xFFFFFFFF) << 32);

		/*
		 * Canonical-check before the value is used for anything. CVE-2014-3610
		 * is this exact bug in KVM: an emulated WRMSR let a guest put a
		 * non-canonical address into LSTAR and reach a real wrmsr, faulting
		 * the host. Here the value never reaches hardware; the check keeps a
		 * bad value out of the trampoline. #GP is not injected: the write is
		 * refused and logged.
		 */
		if (!lstar_canonical(val)) {
			printc("[vmm] LSTAR write %llx is non-canonical, refused\n", (unsigned long long)val);
			goto done;
		}
		vcpu->lstar_shadow = val;
		if (VM_GUEST_PTI) {
			printc("[vmm] LSTAR write %llx on a PTI guest: no trampoline, fallback not yet available\n",
			       (unsigned long long)val);
			VM_PANIC(vcpu);
		}
		if (!vcpu->tramp_installed) {
			guest_tramp_install(vcpu, val);
			vcpu->tramp_installed = 1;
		} else {
			/* A later write only retargets the jump; the VMM writes through its own mapping. */
			*(u64_t *)((u8_t *)GPA2HVA(TRAMP_GPA_PAGE, vcpu->vm) + 6) = val;
		}
		goto done;
	}
	case MSR_IA32_STAR:
	{
		/*
		 * The host GDT follows Linux's selector layout, so a Linux guest's
		 * STAR (0x0023001000000000) is the host's (COS_HOST_STAR) and the
		 * register is left alone at every exit. Another value cannot be given
		 * to the guest until the fallback swap (R16).
		 */
		u64_t val = (regs->ax & 0xFFFFFFFF) | ((regs->dx & 0xFFFFFFFF) << 32);

		vcpu->star_shadow = val;
		if (val != COS_HOST_STAR) {
			printc("[vmm] STAR write %llx differs from the host's %llx: no fallback yet\n",
			       (unsigned long long)val, (unsigned long long)COS_HOST_STAR);
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
