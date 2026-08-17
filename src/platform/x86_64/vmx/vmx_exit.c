#include <chal_cpu.h>
#include <thd.h>
#include <cpuid.h>
#include <chal_asm_inc.h>
#include <vmx.h>
#include <vmx_msr.h>
#include <vmx_logging.h>
#include <vmx_exit.h>
#include <vmx_utils.h>
#include <vmx_vmcs.h>
#include <vmx_exit.h>
#include <inv.h>

void lapic_ack(void);
void restore_from_vm(void);
int cap_thd_switch(struct pt_regs *regs, struct thread *curr, struct thread *next,
			struct comp_info *ci, struct cos_cpu_local_info *cos_info);

int expended_process(struct pt_regs *regs, struct thread *thd_curr, struct comp_info *ci,
                 struct cos_cpu_local_info *cos_info, int timer_intr_context);

/* The tmp regs stack used for VM-exit switching to other threads */
static struct pt_regs tmp_regs[NUM_CPU];

/*
 * Per-CPU cache of host MSR values. These are constant per-core and
 * populated on the first generic VM exit. The vmcall fast path uses
 * these instead of reading from the per-thread vcpu_ctx.
 *
 * Not static: the assembly-only MSR-swap benchmark path in
 * vmx_exit_asm.S reads host_msr_cache[0] directly (NUM_CPU == 1 in
 * this build, so no per-cpu indexing is done there).
 */
struct host_msr_cache {
	u64_t gs_base;
	u64_t gskernel_base;
	u64_t tsc_aux;
	u64_t star;
	u64_t lstar;
	u64_t cstar;
	u64_t fmask;
	int   initialized;
};
struct host_msr_cache host_msr_cache[NUM_CPU];

/* When VMM tries to write cr0/cr4, kernel needs to take care of them */
extern u64_t cr4_fixed1_bits;
extern u64_t cr4_fixed0_bits;
extern u64_t cr0_fixed1_bits;
extern u64_t cr0_fixed0_bits;

int
posted_intr_handler(struct pt_regs *regs)
{
	/* should not come here by design */
	assert(0);
	return 0;
}

static void
posted_intr_inject(void)
{
	chal_selfipi(HW_LAPIC_POSTED_INTR);
}

/*
 * Fast-path VM IPC resume: called from sret_ret when a VM vCPU returns
 * from an IPC. Only restores guest MSRs, sets the
 * return value in rax, and vmresumes.
 *
 * Matches native IPC: single return value in rax.
 */
void
vmx_ipc_resume(struct thread *thd, struct pt_regs *regs)
{
	/* Which is cheaper to get it through arguments or look it up? */
	//struct thread *thd = thd_current(cos_cpu_local_info());

	/* Restore guest MSRs (no msr_get — host values are in per-CPU cache) */
	msr_set(IA32_GS_BASE, thd->vcpu_ctx.vmcs.guest_msr_gs_base);
	msr_set(IA32_KERNEL_GSBASE, thd->vcpu_ctx.vmcs.guest_msr_gskernel_base);
	msr_set(IA32_TSC_AUX, thd->vcpu_ctx.vmcs.guest_tsc_aux);
	msr_set(IA32_STAR, thd->vcpu_ctx.vmcs.guest_star);
	msr_set(IA32_LSTAR, thd->vcpu_ctx.vmcs.guest_lstar);
	msr_set(IA32_CSTAR, thd->vcpu_ctx.vmcs.guest_cstar);
	msr_set(IA32_FMASK, thd->vcpu_ctx.vmcs.guest_fmask);

	/*
	 * Restore all GPs from pt_regs and vmresume.
	 */
	__asm__ __volatile__(
				"movq %%rax, %%rsp\n\t"		\
				"addq $0x20, %%rsp\n\t" 	\
				"popq %%r15\n\t"		\
				"popq %%r14\n\t"		\
				"popq %%r13\n\t"		\
				"popq %%r12\n\t"		\
				"popq %%r11\n\t"		\
				"popq %%r10\n\t"		\
				"popq %%r9\n\t"			\
				"popq %%r8\n\t"			\
				"popq %%rbx\n\t"		\
				"popq %%rcx\n\t"		\
				"popq %%rdx\n\t"		\
				"popq %%rsi\n\t"		\
				"popq %%rdi\n\t"		\
				"popq %%rbp\n\t"		\
				"popq %%rax\n\t"		\
				"addq $0x30, %%rsp\n\t"		\
				"vmresume\n\t"
				:
				: "a"(regs)
				:);

	/* vmresume should not return */
	vmx_assert(0);
}

void
vmx_resume(struct thread *thd)
{
	struct vm_vcpu_shared_region *shared_region;
	u64_t val;

	if (unlikely(thd->vcpu_ctx.state == VM_THD_STATE_STOPPED)) return;
	vmx_assert(thd->vcpu_ctx.state == VM_THD_STATE_RUNNING);

	shared_region = thd->vm_vcpu_shared_region;

	/* Used for VMM manages virtual lapic interrupts */
	if (shared_region->interrupt_status) {
		vmwrite(GUEST_INTERRUPT_STATUS, shared_region->interrupt_status);
	}

	/* User level VMM is responsible to set vcpu's state like ip and sp, or a VM-exit will happen, the kernel should be safe */
	vmwrite(GUEST_RIP, shared_region->ip);
	vmwrite(GUEST_RSP, shared_region->sp);
	vmwrite(GUEST_IA32_EFER, shared_region->efer);

	/* TODO: maybe can put the fixed bits value to the VMM */
	val = shared_region->cr0;
	val |= cr0_fixed1_bits;
	val &= cr0_fixed0_bits;
	vmwrite(GUEST_CR0, val);

	val = shared_region->cr4;
	val |= cr4_fixed1_bits;
	val &= cr4_fixed0_bits;
	vmwrite(GUEST_CR4, val);

	/* This is awkward here, but necessary as the manual mentions that long-mode status should be put into vm entry ctl */
	if (unlikely(shared_region->efer & BIT(10))) {
		u64_t vm_entry_ctls = vmread(VM_ENTRY_CONTROLS);
		vm_entry_ctls |= BIT(9);
		vmwrite(VM_ENTRY_CONTROLS, vm_entry_ctls);
	}


#if VMX_SUPPORT_POSTED_INTR
	/* 
	 * TODO: We definitely would like to support posted interrupt mechanism since it can reduce multi-core
	 * interrupt injection overhead. However, this currently cannot work small timer tick like 100us set up.
	 * Thus, currently just keep the legacy interrupt injection mechanism.
	 */
	if (unlikely(shared_region->inject_evt)) {
		posted_intr_inject();
		shared_region->inject_evt = 0;
	}
#endif

	/* TODO: some msrs like kernel gs are (per-core) constant, can make host saving code simpler`*/
	thd->vcpu_ctx.vmcs.host_msr_gs_base = msr_get(IA32_GS_BASE);
	thd->vcpu_ctx.vmcs.host_msr_gskernel_base = msr_get(IA32_KERNEL_GSBASE);
	thd->vcpu_ctx.vmcs.host_tsc_aux = msr_get(IA32_TSC_AUX);
	thd->vcpu_ctx.vmcs.host_star = msr_get(IA32_STAR);
	thd->vcpu_ctx.vmcs.host_lstar = msr_get(IA32_LSTAR);
	thd->vcpu_ctx.vmcs.host_cstar = msr_get(IA32_CSTAR);
	thd->vcpu_ctx.vmcs.host_fmask = msr_get(IA32_FMASK);

	msr_set(IA32_GS_BASE, thd->vcpu_ctx.vmcs.guest_msr_gs_base);
	msr_set(IA32_KERNEL_GSBASE, thd->vcpu_ctx.vmcs.guest_msr_gskernel_base);
	msr_set(IA32_TSC_AUX, thd->vcpu_ctx.vmcs.guest_tsc_aux);
	msr_set(IA32_STAR, thd->vcpu_ctx.vmcs.guest_star);
	msr_set(IA32_LSTAR, thd->vcpu_ctx.vmcs.guest_lstar);
	msr_set(IA32_CSTAR, thd->vcpu_ctx.vmcs.guest_cstar);
	msr_set(IA32_FMASK, thd->vcpu_ctx.vmcs.guest_fmask);

	/* Restore GPs for vcpu */
	__asm__ __volatile__(
				"movq %%rax, %%rsp\n\t"		\
				"popq %%rax\n\t"		\
				"movq %%rax, %%cr2\n\t"		\
				"popq %%r15\n\t"		\
				"popq %%r14\n\t"		\
				"popq %%r13\n\t"		\
				"popq %%r12\n\t"		\
				"popq %%r11\n\t"		\
				"popq %%r10\n\t"		\
				"popq %%r9\n\t"			\
				"popq %%r8\n\t"			\
				"popq %%rbx\n\t"		\
				"popq %%rcx\n\t"		\
				"popq %%rdx\n\t"		\
				"popq %%rsi\n\t"		\
				"popq %%rdi\n\t"		\
				"popq %%rbp\n\t"		\
				"popq %%rax\n\t"		\
				"vmresume\n\t"			\
				: 
				: "a"(shared_region)
				:);

	/* TODO: what if somehow vmresume fails? */
	vmx_assert(0);
}

static int 
timer_process(struct pt_regs *regs, struct thread *thd_curr)
{
	struct cos_cpu_local_info *cos_info;
	struct comp_info *         comp;
	unsigned long              ip, sp;
	cycles_t                   now;

	cos_info = cos_cpu_local_info();
	vmx_assert(cos_info);	
	comp = &thd_curr->invstk[thd_curr->invstk_top].comp_info;
	vmx_assert(comp);
	return expended_process(regs, thd_curr, comp, cos_info, 0);
}

/*
 * VMCALL fast-path C handler: called from vmx_exit_handler_asm when
 * the exit reason is VMCALL (18). Receives IPC arguments using the
/*
 * VMCALL fast-path C handler: called from vmx_exit_handler_asm when
 * the exit reason is VMCALL (18).
 *
 * Receives a pointer to the fully saved guest registers (pt_regs style)
 * so we can memcpy them to shared_region for vmx_ipc_resume to restore.
 *
 * Args:
 *   regs: pt_regs struct with guest GP registers.
 *
 * Native Composite IPC register convention is decoded from regs:
 *   cap_encoded = guest rax: (cap + 1) << 16  (same as native sinv)
 *   arg1 = guest rbx, arg2 = guest rsi, arg3 = guest rdi, arg4 = guest rdx
 */

// Put them into same elf section?
void
vmx_vmcall_fast_handler(struct pt_regs *regs)
{
	int ret = 0;
	unsigned long cpuid;
	struct cos_cpu_local_info *cos_info;
	struct thread *thd_curr;
	struct comp_info *curr_ci;
	struct cap_sinv  *sinvc;
	struct host_msr_cache *cache;
	capid_t cap;

	cos_info = cos_cpu_local_info();
	thd_curr = thd_current(cos_info);
	cpuid = cos_info->cpuid;
	cache = &host_msr_cache[cpuid];

	/* Decode sinv cap using same formula as __userregs_getcap */
	cap = (regs->ax >> COS_CAPABILITY_OFFSET) - 1;
	thd_curr->vcpu_ctx.state = VM_THD_STATE_VMCALL_FAST;
	/*
	 * Save current guest MSRs before overwriting them.
	 * If we don't save them here, vmx_ipc_resume will restore stale values,
	 * causing GS_BASE corruption and crashes in guest OSes like Linux.
	 */
	thd_curr->vcpu_ctx.vmcs.guest_msr_gs_base = msr_get(IA32_GS_BASE);
	thd_curr->vcpu_ctx.vmcs.guest_msr_gskernel_base = msr_get(IA32_KERNEL_GSBASE);
	/*thd_curr->vcpu_ctx.vmcs.guest_tsc_aux = msr_get(IA32_TSC_AUX);
	thd_curr->vcpu_ctx.vmcs.guest_star = msr_get(IA32_STAR);
	thd_curr->vcpu_ctx.vmcs.guest_lstar = msr_get(IA32_LSTAR);
	thd_curr->vcpu_ctx.vmcs.guest_cstar = msr_get(IA32_CSTAR);
	thd_curr->vcpu_ctx.vmcs.guest_fmask = msr_get(IA32_FMASK);*/
	/* Look up the sinv cap and target comp_info */
	unsigned long ip, sp;
	curr_ci = thd_invstk_current(thd_curr, &ip, &sp, cos_info);
	if (unlikely(!curr_ci)) {
		printk("cos: vmcall fast: no comp_info\n");
		regs->ax = (word_t)-EINVAL;
		vmwrite(GUEST_RIP, regs->r8);
		return;
	}

	sinvc = (struct cap_sinv *)captbl_lkup(curr_ci->captbl, cap);
	if (unlikely(!sinvc || sinvc->h.type != CAP_SINV)) {
		printk("cos: vmcall fast: no sinv at cap %lu\n", (unsigned long)cap);
		regs->ax = (word_t)-EINVAL;
		vmwrite(GUEST_RIP, regs->r8);
		return;
	}

	/*
	 * Restore host MSRs from per-CPU cache (constant per core).
	 * We must do this before server execution because the server
	 * uses syscall, which depends on host STAR/LSTAR/FMASK/GS_BASE.
	 */
	msr_set(IA32_GS_BASE, cache->gs_base);
	msr_set(IA32_KERNEL_GSBASE, cache->gskernel_base);
	msr_set(IA32_TSC_AUX, cache->tsc_aux);
	msr_set(IA32_STAR, cache->star);
	msr_set(IA32_LSTAR, cache->lstar);
	msr_set(IA32_CSTAR, cache->cstar);
	msr_set(IA32_FMASK, cache->fmask);

	/* Only GUEST_RIP needs updating (r9 holds the return address) */
	vmwrite(GUEST_RIP, regs->r9);

	/* for measurement vmcall_msrs comment out 
		ln311(vmwrite(GUEST_RIP, regs->r9)) and ln266 (thd_curr->vcpu_ctx.state = VM_THD_STATE_VMCALL_FAST)
	u64_t inst_length = vmread(EXIT_INSTRUCTION_LENGTH);
	u64_t guest_rip = vmread(GUEST_RIP);
	guest_rip = guest_rip + inst_length;
	vmwrite(GUEST_RIP, guest_rip);
	vmx_ipc_resume(thd_curr, regs);
	*/

	sinv_call(thd_curr, sinvc, regs, cos_info);	
	/* no thread switch → restore_from_vm → sysretq */
	__asm__ volatile("movq %%rbx, %%rsp; jmpq *%%rcx":
			 : "a"(ret), "b"(regs),"c"(&restore_from_vm));
	/* Should never come here */
	vmx_assert(0);
}

void
vmx_exit_handler(struct vm_vcpu_shared_region *regs)
{
	u64_t reason, qualification, gla, gpa, inst_length, inst_info;
	u8_t reason_nr;
	int ret = 0;
	unsigned long cpuid;
	struct cos_cpu_local_info *cos_info;
	struct thread *thd_curr, *thd_exception_handler, *next;
	struct vm_vcpu_shared_region *shared_region;
	cos_info = cos_cpu_local_info();
	thd_curr = thd_current(cos_info);	
	cpuid = cos_info->cpuid;

	vmx_assert(cos_info);
	vmx_assert(thd_curr && thd_curr->cpuid == get_cpuid());
	vmx_assert(thd_curr->thd_type == THD_TYPE_VM);

	thd_exception_handler = thd_curr->exception_handler;

	/*
	 * Generic exit path only — VMCALL is handled by vmx_vmcall_fast_handler
	 * via the asm fast path and never reaches here.
	 */
	reason = vmread(EXIT_REASON);
	reason_nr = reason & 0xffff;
	vmx_assert(reason_nr < MAX_VM_EXIT_REASONS);
	VMX_DEBUG("VM thd: %u on core: %u get VM-exit (reason: ) on handler: %u\n", thd_curr->tid, cos_info->cpuid, reason_nr, thd_exception_handler->tid);

	/*
	 * Full MSR save/restore for generic exits.
	 * Also populates the per-CPU host MSR cache on first exit.
	 */
	thd_curr->vcpu_ctx.vmcs.guest_msr_gs_base = msr_get(IA32_GS_BASE);
	thd_curr->vcpu_ctx.vmcs.guest_msr_gskernel_base = msr_get(IA32_KERNEL_GSBASE);
	thd_curr->vcpu_ctx.vmcs.guest_tsc_aux = msr_get(IA32_TSC_AUX);
	thd_curr->vcpu_ctx.vmcs.guest_star = msr_get(IA32_STAR);
	thd_curr->vcpu_ctx.vmcs.guest_lstar = msr_get(IA32_LSTAR);
	thd_curr->vcpu_ctx.vmcs.guest_cstar = msr_get(IA32_CSTAR);
	thd_curr->vcpu_ctx.vmcs.guest_fmask = msr_get(IA32_FMASK);

	msr_set(IA32_GS_BASE, thd_curr->vcpu_ctx.vmcs.host_msr_gs_base);
	msr_set(IA32_KERNEL_GSBASE, thd_curr->vcpu_ctx.vmcs.host_msr_gskernel_base);
	msr_set(IA32_TSC_AUX, thd_curr->vcpu_ctx.vmcs.host_tsc_aux);
	msr_set(IA32_STAR, thd_curr->vcpu_ctx.vmcs.host_star);
	msr_set(IA32_LSTAR, thd_curr->vcpu_ctx.vmcs.host_lstar);
	msr_set(IA32_CSTAR, thd_curr->vcpu_ctx.vmcs.host_cstar);
	msr_set(IA32_FMASK, thd_curr->vcpu_ctx.vmcs.host_fmask);

	/* Cache host MSR values per-CPU (they're constant per core) */
	if (unlikely(!host_msr_cache[cpuid].initialized)) {
		host_msr_cache[cpuid].gs_base       = thd_curr->vcpu_ctx.vmcs.host_msr_gs_base;
		host_msr_cache[cpuid].gskernel_base = thd_curr->vcpu_ctx.vmcs.host_msr_gskernel_base;
		host_msr_cache[cpuid].tsc_aux       = thd_curr->vcpu_ctx.vmcs.host_tsc_aux;
		host_msr_cache[cpuid].star          = thd_curr->vcpu_ctx.vmcs.host_star;
		host_msr_cache[cpuid].lstar         = thd_curr->vcpu_ctx.vmcs.host_lstar;
		host_msr_cache[cpuid].cstar         = thd_curr->vcpu_ctx.vmcs.host_cstar;
		host_msr_cache[cpuid].fmask         = thd_curr->vcpu_ctx.vmcs.host_fmask;
		host_msr_cache[cpuid].initialized   = 1;
	}

	qualification = vmread(EXIT_QUALIFICATION);
	gla = vmread(EXIT_GUEST_LINEAR_ADDRESS);
	gpa = vmread(EXIT_GUEST_PHYSICAL_ADDRESS);
	inst_length = vmread(EXIT_INSTRUCTION_LENGTH);
	inst_info = vmread(EXIT_INSTRUCTION_INFORMATION);

	shared_region = thd_curr->vm_vcpu_shared_region;

	/* Share GPs with VMM */
	memcpy(shared_region, regs, sizeof(struct vm_vcpu_shared_region));

	shared_region->reason = reason_nr;
	shared_region->ip = vmread(GUEST_RIP);
	shared_region->sp = vmread(GUEST_RSP);
	shared_region->efer = vmread(GUEST_IA32_EFER);
	shared_region->cr0 = vmread(GUEST_CR0);
	shared_region->cr3 = vmread(GUEST_CR3);
	shared_region->cr4 = vmread(GUEST_CR4);
	shared_region->interrupt_status = vmread(GUEST_INTERRUPT_STATUS);
	shared_region->inst_length = inst_length;
	shared_region->gpa = gpa;
	shared_region->qualification = qualification;

	/* FIXME: handle vcpu execution time correctly, this also seems should add into VMM thread */
	thd_curr->exec += 1000;

	/* Save fs for vcpu, this will be restoed later in the thread switch path, thus safe */
	thd_curr->tls = msr_get(IA32_FS_BASE);

	if (reason_nr == VM_EXIT_REASON_EXTERNAL_INTERRUPT) {
		u64_t intr_info;
		u8_t vector;

		/* Read the interrupt vector that caused the VM exit */
		intr_info = vmread(EXIT_INTERRUPTION_INFO);
		vector = intr_info & 0xFF; /* Bits 7:0 contain the vector */

		lapic_ack();

		/* Dispatch based on the actual interrupt vector */
		switch (vector) {
		case HW_LAPIC_TIMER:
			/*FIXME: might need a xsave/xrestore for sse/avx for current vcpu thd because current thd is set to be exception handler */
			copy_gp_regs(&thd_exception_handler->regs, &tmp_regs[cpuid]);
			ret = timer_process(&tmp_regs[cpuid], thd_exception_handler);
			break;

		case HW_LAPIC_IPI_ASND:
			/* TODO: IPI handling during VM execution - not yet implemented/tested */
			VMX_DEBUG("IPI interrupt (vector %d) during VM execution - not yet supported\n", vector);
			vmx_assert(0);
			break;

		case HW_LAPIC_POSTED_INTR:
			/* TODO: Posted interrupt handling - not yet implemented/tested */
			VMX_DEBUG("Posted interrupt (vector %d) during VM execution - not yet supported\n", vector);
			vmx_assert(0);
			break;

		default:
			/* Unexpected interrupt vector during VM execution */
			VMX_DEBUG("Unexpected interrupt vector %d during VM execution\n", vector);
			vmx_assert(0);
			break;
		}
	} else {
		next = thd_exception_handler;
		ret = cap_thd_switch(&tmp_regs[cpuid], thd_curr, next, NULL, cos_info);
		vmx_assert(ret == 0);
	}

	__asm__ volatile("movq %%rbx, %%rsp; jmpq *%%rcx": : "a"(ret), "b"(&tmp_regs[cpuid]),"c"(&restore_from_vm));

	/* Should never come here */
	vmx_assert(0);
}
