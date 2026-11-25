/*
 * w11 Phase 2: the VMM's page-table lock (VMCALL_PT_LOCK). pt_lock.c.
 */
#ifndef PT_LOCK_H
#define PT_LOCK_H

#include <cos_types.h>
#include <vmrt.h>

#if VMCALL_PT_LOCK
void pt_lock_init(compid_t vm, compid_t proc_comp);
/* 1 when the violation was a locked page's (handled, RIP not advanced), 0 to fall through */
int  pt_lock_ept_violation(struct vmrt_vm_vcpu *vcpu);
/* ERRAND_ID_OP_REGISTER_LOCKED: lock every page, then register. 0 or a negative errno. */
long pt_lock_register(struct vmrt_vm_vcpu *vcpu, unsigned long root, unsigned long root2, unsigned long list_gpa);
/* After an explicit unregister of `root`: drop the second root and unlock the pages. */
/* Phase 3: REGISTER_GATE (rdx = a struct errand_id_gate_page) */
long pt_lock_register_gate(struct vmrt_vm_vcpu *vcpu, unsigned long root, unsigned long root2, unsigned long list_gpa);
void pt_lock_set_comps(compid_t c0, compid_t c1, compid_t c2);
void pt_lock_unregistered(struct vmrt_vm_vcpu *vcpu, unsigned long root);
void pt_lock_dump(void);
#endif

#endif /* PT_LOCK_H */
