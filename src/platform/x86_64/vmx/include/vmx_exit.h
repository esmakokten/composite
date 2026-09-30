#pragma once

struct vm_vcpu_shared_region;
struct thread;

void vmx_exit_handler_asm(void);
void vmx_resume(struct thread *thd);
void vmx_exit_handler(struct vm_vcpu_shared_region *regs);

/*
 * The fallback swap, writes only, from the kernel's own record: the guest's
 * diverged values at entry, the host's constants at exit. Behind one unlikely
 * test on both paths; a Linux guest without page-table isolation never sets
 * a bit.
 */
#include <vmx_vmcs.h>
#include <vmx_msr.h>
#include <chal_asm_inc.h>
#include <shared/vm_host_msr.h>

static inline void
vmx_fallback_load_guest(struct vmx_vmcs *vmcs)
{
	if (vmcs->fb_mask & VMX_FB_STAR)  msr_set(IA32_STAR, vmcs->fb_star);
	if (vmcs->fb_mask & VMX_FB_LSTAR) msr_set(IA32_LSTAR, vmcs->fb_lstar);
	if (vmcs->fb_mask & VMX_FB_FMASK) msr_set(IA32_FMASK, vmcs->fb_fmask);
}

static inline void
vmx_fallback_load_host(struct vmx_vmcs *vmcs)
{
	if (vmcs->fb_mask & VMX_FB_STAR)  msr_set(IA32_STAR, COS_HOST_STAR);
	if (vmcs->fb_mask & VMX_FB_LSTAR) msr_set(IA32_LSTAR, COS_SYSCALL_TRAMP_VA);
	if (vmcs->fb_mask & VMX_FB_FMASK) msr_set(IA32_FMASK, COS_HOST_FMASK);
}
