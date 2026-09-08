#include <vmrt.h>
#include <vmx_msr.h>

/*
 * ---------------------------------------------------------------------------
 * IA32_LSTAR interception and the guest SYSCALL trampoline.
 *
 * IA32_LSTAR permanently holds Composite's COS_SYSCALL_TRAMP_VA, so the vmcall
 * fast path never saves or restores it. For that to be correct the guest's own
 * SYSCALL must still reach entry_SYSCALL_64, so:
 *
 *   - the guest's LSTAR write is intercepted and absorbed into a per-vCPU
 *     shadow; hardware is never touched (a guest read returns the shadow);
 *   - at that same intercept we build a one-page trampoline in the guest at
 *     COS_SYSCALL_TRAMP_VA holding an indirect jump to the value the guest
 *     just tried to write. The design is self-calibrating: the intercepted
 *     value IS how we learn the target, so no build-time agreement with the
 *     guest is needed and the guest binary is untouched.
 *
 * Nothing is shared with Composite but the number: different page tables,
 * different physical frames.
 * ---------------------------------------------------------------------------
 */
#define COS_SYSCALL_TRAMP_VA  0xfffffe8000000000ULL
#define TRAMP_PGD_IDX         509     /* (VA >> 39) & 511 */

/*
 * Frames for the trampoline and its page-table chain, taken from guest-physical
 * memory the VMM has mapped but never told the guest about. The VM is created
 * with GUEST_MEM_SZ (310MB), while the memory map the guest is handed ends much
 * earlier:
 *
 *   BIOS-e820: [mem 0x0000000000000000-0x000000000009dfff] usable
 *   BIOS-e820: [mem 0x000000000009e000-0x00000000000fffff] reserved
 *   BIOS-e820: [mem 0x0000000000100000-0x0000000003cfffff] usable
 *
 * so everything above 0x3cfffff is backed and EPT-mapped but outside every
 * range Linux knows about. That matters twice over. First, this intercept fires
 * from syscall_init() (init/main.c:992), before mm_init() at 993, so the guest
 * has no page allocator and every frame must come from us. Second, the frame is
 * mapped execute-only in EPT, so the guest must never READ it either -- and the
 * e820 "reserved" window below 1MB is not good enough for that: it is the BIOS
 * ROM and DMI area, which Linux scans at boot (dmi_scan_machine from
 * setup_arch), taking an EPT violation. Memory the guest was never told about
 * is not probed at all.
 */
#define TRAMP_GPA_PAGE  0x04000000ULL
#define TRAMP_GPA_PUD   0x04001000ULL
#define TRAMP_GPA_PMD   0x04002000ULL
#define TRAMP_GPA_PT    0x04003000ULL

#define PTE_P  (1ULL << 0)
#define PTE_W  (1ULL << 1)
#define PTE_G  (1ULL << 8)	/* _PAGE_GLOBAL */

#define CR3_ADDR_MASK 0x000ffffffffff000ULL

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

	/* Supervisor-only and not writable; SYSCALL raises CPL to 0 before the fetch. */
	/*
	 * Global. Linux clears _PAGE_GLOBAL broadly under PTI (mm/init.c) and then
	 * deliberately sets it back on exactly the pages it shares between the
	 * kernel and user PGDs (mm/pti.c). This trampoline is that class of page,
	 * so global matches Linux's own convention rather than fighting it, and it
	 * is the right default for the PTI work later.
	 */
	pt[0]  = TRAMP_GPA_PAGE | PTE_P | PTE_G;
	pmd[0] = TRAMP_GPA_PT   | PTE_P | PTE_W;
	pud[0] = TRAMP_GPA_PMD  | PTE_P | PTE_W;

	pgd[TRAMP_PGD_IDX] = TRAMP_GPA_PUD | PTE_P | PTE_W;

	/*
	 * DELIBERATELY NOT DONE HERE: the PTI user-half entry.
	 *
	 * SYSCALL does not switch CR3, so on a PTI guest the first instruction
	 * after it runs on the USER page tables, and this trampoline would also
	 * have to be installed at pgd[512 + TRAMP_PGD_IDX] -- the second page of
	 * the 8K PGD pair -- because pti_init() clones only cpu_entry_area,
	 * .entry.text, espfix and vsyscall, not index 509.
	 *
	 * PTI support is deferred. Writing the user half on a NON-PTI guest would
	 * be actively harmful: there is no second page in the PGD allocation, so
	 * the write would land in unrelated memory. The one line to add later is
	 *
	 *     pgd[512 + TRAMP_PGD_IDX] = pgd[TRAMP_PGD_IDX];
	 *
	 * guarded on the guest actually having PTI. clone_pgd_range() then carries
	 * both halves into every forked process, since 509 >= KERNEL_PGD_BOUNDARY.
	 */

	printc("[vmm] LSTAR intercept: guest target %llx -> trampoline at %llx "
	       "(gpa %llx, pgd[%d], kernel half only)\n",
	       (unsigned long long)target, (unsigned long long)COS_SYSCALL_TRAMP_VA,
	       (unsigned long long)TRAMP_GPA_PAGE, TRAMP_PGD_IDX);
}

void 
rdmsr_handler(struct vmrt_vm_vcpu *vcpu)
{
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;

	switch (regs->cx) {
	case MSR_IA32_LSTAR:
	{
		/* Served from the shadow; hardware holds Composite's trampoline VA. */
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
	case MSR_IA32_LSTAR:
	{
		u64_t val = (regs->ax & 0xFFFFFFFF) | ((regs->dx & 0xFFFFFFFF) << 32);

		/*
		 * Canonical-check before the value is used for anything. CVE-2014-3610
		 * is this exact bug in KVM: an emulated WRMSR let a guest put a
		 * non-canonical address into LSTAR and reach a real wrmsr, faulting the
		 * host. Here the value never reaches hardware at all, so that class is
		 * gone by construction -- this check keeps a bad value out of the
		 * trampoline as well. Note we do not inject #GP as real hardware would;
		 * the write is refused and logged.
		 */
		if (!lstar_canonical(val)) {
			printc("[vmm] LSTAR write %llx is non-canonical, refused\n",
			       (unsigned long long)val);
			goto done;
		}

		vcpu->lstar_shadow = val;
		if (!vcpu->tramp_installed) {
			guest_tramp_install(vcpu, val);
			vcpu->tramp_installed = 1;
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
	default:
		VM_PANIC(vcpu);;
	}

done:
	GOTO_NEXT_INST(regs);
	return;
}
