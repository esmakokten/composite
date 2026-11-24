/*
 * w11_vmcall_identity: the per-VM process table.
 *
 * A guest process that may make errands is bound to a *process component*: the VM's page
 * table (the EPT) with a capability table of its own. The vmcall fast path masks GUEST_CR3
 * into a root and scans the VM's table for it; a hit hands it the process component's
 * capability table instead of the VM component's.
 *
 * Where the table lives, and why: a static kernel array indexed by the VM component's ASID.
 * struct comp_info cannot grow (its copy fills a 64-byte sinv capability, and cap_comp has 4
 * bytes left), while the fast path already holds the VM's comp_info and so its protdom: the
 * table is found with no extra load. 16 entries of 16 bytes are four cache lines.
 *
 * Invariant 3 (CLAUDE.md): only these operations write the table. A register claims a free
 * entry, writes the component pointer, then the root; an unregister clears the root first.
 * The fast path reads the root, then the pointer.
 */
#ifndef VM_PROC_H
#define VM_PROC_H

#include "shared/cos_types.h"
#include "component.h"
#include "captbl.h"

/* The kernel has no shared volatile-access helper; these two are all the table needs. */
#define VM_PROC_ACCESS(x) (*(volatile typeof(x) *)&(x))
#define VM_PROC_BARRIER() __asm__ __volatile__("" ::: "memory")

/* Never a masked root (its low 12 bits are clear): marks an entry being written. */
#define VM_PROC_ROOT_CLAIMED 1UL

struct vm_proc_ent {
	unsigned long     root; /* 0 = free */
	struct comp_info *ci;
};

struct vm_proc_tbl {
	struct vm_proc_ent ent[VM_PROC_MAX];
} __attribute__((aligned(CACHELINE_SIZE)));

extern struct vm_proc_tbl vm_proc_tbls[VM_PROC_MAX_VMS];

static inline struct vm_proc_tbl *
vm_proc_tbl_get(prot_domain_t protdom)
{
	/* An ASID below the base wraps to a large index and is refused too. */
	unsigned long idx = (unsigned long)PROTDOM_ASID(protdom) - VM_PROC_ASID_BASE;

	if (unlikely(idx >= VM_PROC_MAX_VMS)) return NULL;
	return &vm_proc_tbls[idx];
}

/* The fast-path lookup: no printk, no stores. root is already masked. */
static inline struct comp_info *
vm_proc_lkup(struct vm_proc_tbl *t, unsigned long root)
{
	int i;

	if (unlikely(!root)) return NULL;
	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->ent[i].root) == root) {
			VM_PROC_BARRIER();
			return VM_PROC_ACCESS(t->ent[i].ci);
		}
	}
	return NULL;
}

static inline int
__vm_proc_comps(struct captbl *ct, capid_t vm_comp_cap, struct vm_proc_tbl **t, struct cap_comp **vmc)
{
	struct cap_comp *c = (struct cap_comp *)captbl_lkup(ct, vm_comp_cap);

	if (unlikely(!CAP_TYPECHK(c, CAP_COMP))) return -EINVAL;
	*t = vm_proc_tbl_get(c->info.pgtblinfo.protdom);
	if (unlikely(!*t)) return -EINVAL;
	*vmc = c;

	return 0;
}

static inline int
vm_proc_register(struct captbl *ct, capid_t vm_comp_cap, capid_t proc_comp_cap, unsigned long root)
{
	struct vm_proc_tbl *t;
	struct cap_comp    *vmc, *pc;
	int                 i, ret;

	if (unlikely(!root || (root & ~VM_PROC_ROOT_MASK))) return -EINVAL;
	if ((ret = __vm_proc_comps(ct, vm_comp_cap, &t, &vmc))) return ret;
	pc = (struct cap_comp *)captbl_lkup(ct, proc_comp_cap);
	if (unlikely(!CAP_TYPECHK(pc, CAP_COMP))) return -EINVAL;
	/* A process component runs on its VM's page table (the EPT), nothing else. */
	if (unlikely(pc->info.pgtblinfo.pgtbl != vmc->info.pgtblinfo.pgtbl)) return -EINVAL;

	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->ent[i].root) == root) return -EEXIST;
	}
	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->ent[i].root) != 0) continue;
		if (cos_cas(&t->ent[i].root, 0, VM_PROC_ROOT_CLAIMED) != CAS_SUCCESS) continue;
		t->ent[i].ci = &pc->info;
		VM_PROC_BARRIER();
		VM_PROC_ACCESS(t->ent[i].root) = root;

		return 0;
	}

	return -ENOMEM; /* the table is full (the kernel has no ENOSPC) */
}

static inline int
vm_proc_unregister(struct captbl *ct, capid_t vm_comp_cap, unsigned long root)
{
	struct vm_proc_tbl *t;
	struct cap_comp    *vmc;
	int                 i, ret;

	if (unlikely(!root || (root & ~VM_PROC_ROOT_MASK))) return -EINVAL;
	if ((ret = __vm_proc_comps(ct, vm_comp_cap, &t, &vmc))) return ret;
	for (i = 0; i < VM_PROC_MAX; i++) {
		/* The root goes first: from here no fast path can match the entry. */
		if (cos_cas(&t->ent[i].root, root, 0) == CAS_SUCCESS) return 0;
	}

	return -ENOENT;
}

#endif /* VM_PROC_H */
