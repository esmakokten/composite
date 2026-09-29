#include <chal_cpu.h>
#include <thd.h>
#include <fpu.h>
#include <cpuid.h>
#include <chal_asm_inc.h>
#include <vmx.h>
#include <vmx_msr.h>
#include <vmx_logging.h>
#include <vmx_exit.h>
#include <vmx_utils.h>
#include <vmx_vmcs.h>
#include <vmx_exit.h>

void lapic_ack(void);
void restore_from_vm(void);
int cap_thd_switch(struct pt_regs *regs, struct thread *curr, struct thread *next,
			struct comp_info *ci, struct cos_cpu_local_info *cos_info);

int expended_process(struct pt_regs *regs, struct thread *thd_curr, struct comp_info *ci,
                 struct cos_cpu_local_info *cos_info, int timer_intr_context);

/* The tmp regs stack used for VM-exit switching to other threads */
static struct pt_regs tmp_regs[NUM_CPU];

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


void
vmx_resume(struct thread *thd)
{
	struct vm_vcpu_shared_region *shared_region;
	u64_t val;

	if (unlikely(thd->vcpu_ctx.state == VM_THD_STATE_STOPPED)) return;
	vmx_assert(thd->vcpu_ctx.state == VM_THD_STATE_RUNNING);

	shared_region = thd->vm_vcpu_shared_region;

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

	/* Used for VMM manages virtual lapic interrupts */
	if (shared_region->interrupt_status) {
		vmwrite(GUEST_INTERRUPT_STATUS, shared_region->interrupt_status);
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

	/*
	 * IA32_GS_BASE is not swapped here: the VMCS does it. A VM entry loads
	 * GS.base from GUEST_GS_BASE and a VM exit stores it there and loads
	 * HOST_GS_BASE, which holds this core's constant (the kernel stack
	 * info). The guest's writes are passed through and land in the guest
	 * field at the exit, so no software copy is needed in either direction.
	 */
	/*
	 * IA32_KERNEL_GS_BASE is not swapped here. Its guest value stays loaded
	 * across exits; the only event that needs it is a different vCPU being
	 * entered on this core, and vmx_owner_switch() handles that. The host
	 * never uses the register outside the swapgs bracket (see the invariant
	 * at vmx_thd_start_or_resume).
	 */
	/* TSC_AUX: stays loaded; writes are intercepted; the owner switch handles a change of vCPU. */
	thd->vcpu_ctx.vmcs.host_star = msr_get(IA32_STAR);
	thd->vcpu_ctx.vmcs.host_lstar = msr_get(IA32_LSTAR);
	/*
	 * CSTAR: intercepted both ways, shadowed in the VMM, never loaded.
	 * FMASK: intercepted both ways; the guest's value coincides with the
	 * host's (COS_HOST_FMASK), so the register is never touched.
	 */

	msr_set(IA32_STAR, thd->vcpu_ctx.vmcs.guest_star);
	msr_set(IA32_LSTAR, thd->vcpu_ctx.vmcs.guest_lstar);

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

	/* Evaluation hooks; empty in the timing flavour. The null exit does not return. */

	/* The exit just reloaded CR0 from the HOST_CR0 snapshot; resync the FPU shadow. */
	fpu_shadow_resync();

	/* GS_BASE: swapped by the VMCS; KERNEL_GS_BASE and TSC_AUX: stay loaded, see vmx_resume(). */
	thd_curr->vcpu_ctx.vmcs.guest_star = msr_get(IA32_STAR);
	thd_curr->vcpu_ctx.vmcs.guest_lstar = msr_get(IA32_LSTAR);
	
	msr_set(IA32_STAR, thd_curr->vcpu_ctx.vmcs.host_star);
	msr_set(IA32_LSTAR, thd_curr->vcpu_ctx.vmcs.host_lstar);

	vmx_assert(cos_info);
	vmx_assert(thd_curr && thd_curr->cpuid == get_cpuid());
	vmx_assert(thd_curr->thd_type == THD_TYPE_VM);

	shared_region = thd_curr->vm_vcpu_shared_region;
	thd_exception_handler = thd_curr->exception_handler;
	
	reason = vmread(EXIT_REASON);
	qualification = vmread(EXIT_QUALIFICATION);
	gla = vmread(EXIT_GUEST_LINEAR_ADDRESS);
	gpa = vmread(EXIT_GUEST_PHYSICAL_ADDRESS);
	inst_length = vmread(EXIT_INSTRUCTION_LENGTH);
	inst_info = vmread(EXIT_INSTRUCTION_INFORMATION);

	if (unlikely(reason & (1 << 31))) {
		/* TODO: Should not happen, but need to handle this case correctly later */
		VMX_DEBUG("exit reason:%x(%lu)\n", reason, reason);
		VMX_DEBUG("exit qualification:0x%x\n", qualification);
		vmx_assert(0);
	}

	reason_nr = reason & 0xffff;
	vmx_assert(reason_nr < MAX_VM_EXIT_REASONS);
	VMX_DEBUG("VM thd: %u on core: %u get VM-exit (reason: ) on handler: %u\n", thd_curr->tid, cos_info->cpuid, reason_nr, thd_exception_handler->tid);

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

	/*
	 * No FS_BASE read here. By the first host instruction the exit has
	 * already stored the guest's base into GUEST_FS_BASE and loaded
	 * FS.base from HOST_FS_BASE, so a read returned the host's own value
	 * (checked: EVALCHK in the count flavour). Its one effect was to keep
	 * vcpu->tls equal to HOST_FS_BASE for the TLS compare in
	 * cap_thd_switch(); that value is assigned where HOST_FS_BASE is
	 * written (vmx_host_state_init). Invariant: vcpu->tls == HOST_FS_BASE.
	 */

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
