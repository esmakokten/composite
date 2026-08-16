#pragma once

struct vm_vcpu_shared_region;
struct thread;

/* VM IPC: sinv caps for vmcall-based IPC start at this captbl slot in the VM component */
#define VM_IPC_SINV_CAP_BASE 64
#define VM_IPC_MAX_SINV_CAPS 16

void vmx_exit_handler_asm(void);
void vmx_resume(struct thread *thd);
void vmx_exit_handler(struct vm_vcpu_shared_region *regs);
void vmx_ipc_resume(struct thread *thd, struct pt_regs *regs);
void vmx_vmcall_fast_handler(struct pt_regs *regs);

