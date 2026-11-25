/*
 * w11 Phase 2: the VMM's page-table lock (VMCALL_PT_LOCK). pt_lock.c.
 */
#ifndef PT_LOCK_H
#define PT_LOCK_H

#include <cos_types.h>
#include <vmrt.h>

#if VMCALL_PT_LOCK
void pt_lock_init(compid_t vm);
/* 1 when the violation was a locked page's (handled, RIP not advanced), 0 to fall through */
int  pt_lock_ept_violation(struct vmrt_vm_vcpu *vcpu);
#endif

#endif /* PT_LOCK_H */
