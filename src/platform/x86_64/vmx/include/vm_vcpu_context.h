#pragma once

/* Abstraction layer for vm support*/
#include <vmx_vmcs.h>

typedef enum {
	VM_THD_STATE_STOPPED    = 1,
	VM_THD_STATE_RUNNING    = 1 << 1, 
	VM_THD_STATE_VMCALL_FAST = 1 << 2,
} vm_thd_state_t;

#ifndef VMCALL_IDENTITY
#error "cos_config.h must be included before vm_vcpu_context.h"
#endif

struct vm_proc_tbl;

struct vm_vcpu_context {
	vm_thd_state_t state;
#if VMCALL_IDENTITY
	/*
	 * w11 Phase 5 (A62): the VM's process table, set once by CAPTBL_OP_VM_PROC_ATTACH; NULL refuses
	 * every ring-3 vmcall. Here, in the hole after `state`, it shares a cache line with
	 * vmcs.host_cr3, which the fast path reads anyway (vmx_exit.c, the CR3 policy).
	 */
	struct vm_proc_tbl *proc_tbl;
#endif
	struct vmx_vmcs vmcs;
};

struct thread;
struct cap_vm_vmcb;

void vm_env_init(void);
void vm_thd_init(struct thread *thd, void *vm_pgd, struct cap_vm_vmcb *vmcb);
void vm_thd_exec(struct thread *thd);
