#pragma once

struct vm_vcpu_shared_region;
struct thread;

void vmx_exit_handler_asm(void);
void vmx_exit_for_measurement(void);
void vmx_resume(struct thread *thd);
void vmx_exit_handler(struct vm_vcpu_shared_region *regs);
void vmx_exit_handler_measurement(struct vm_vcpu_shared_region *regs);