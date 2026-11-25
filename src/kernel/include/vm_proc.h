/*
 * w11_vmcall_identity: the per-VM process table.
 *
 * A guest process that may make errands is bound to a *process component*: the VM's page
 * table (the EPT) with a capability table of its own. The vmcall fast path masks GUEST_CR3
 * into a root and scans the VM's table for it; a hit hands it the process component's
 * capability table instead of the VM component's.
 *
 * Where the table lives (Phase 5, DECISIONS A61 to A66): a page of its own, retyped from the
 * capability manager's memory when it creates the VM, behind a CAP_VM_PROC_TBL capability. The
 * table records the VM's EPT root at activation. Each vCPU thread is pointed at it once
 * (CAPTBL_OP_VM_PROC_ATTACH, thd->vcpu_ctx.proc_tbl), and the fast path reaches it from there.
 * Registration and unregistration are operations on the table's capability: holding it is the
 * authority, and no kernel variable names a VM's table.
 *
 * Layout, hot lines first (A64): root[16] on lines 0 and 1; then sixteen {gate, captbl} pairs on
 * lines 2 to 5; then the cold fields. A hit on entries 0 to 7 reads one root line and one pair line.
 *
 * Invariant 3 (CLAUDE.md): only these operations write the table. A register claims a free
 * entry by CAS, writes the pair (and its reference) and then the root; an unregister clears the
 * root first. The fast path reads the root, then the pair.
 *
 * A65: the pair holds the process component's capability table, and the entry holds a reference
 * on that table's capability (the one comp_activate takes), so the table cannot be frozen, and so
 * cannot be freed, while the entry is registered. Deactivating the process component no longer
 * leaves the fast path a dangling pointer. The hit path reads nothing extra for it.
 */
#ifndef VM_PROC_H
#define VM_PROC_H

#include "shared/cos_types.h"
#include "component.h"
#include "captbl.h"
#include "thd.h"

#if VMCALL_IDENTITY /* A67: nothing of this exists with the switch off */

/* The kernel has no shared volatile-access helper; these two are all the table needs. */
#define VM_PROC_ACCESS(x) (*(volatile typeof(x) *)&(x))
#define VM_PROC_BARRIER() __asm__ __volatile__("" ::: "memory")

/* Never a masked root (its low 12 bits are clear): marks an entry being written. */
#define VM_PROC_ROOT_CLAIMED 1UL

struct vm_proc_pair {
	/*
	 * Phase 3 (A33): the one vmcall instruction the entry may be served from, its gate's; 0 = no
	 * gate, never served under VMCALL_GATE. Unused without it.
	 */
	unsigned long  gate;
	struct captbl *ct; /* the process component's capability table: what a hit needs next */
};

struct vm_proc_ctr {
	/* Counted, never printed from the fast path (invariant 1). Racy across vCPUs: diagnostics. */
	unsigned long hits, misses, badslot;
} __attribute__((aligned(CACHELINE_SIZE)));

struct vm_proc_tbl {
	unsigned long       root[VM_PROC_MAX]; /* lines 0, 1; 0 = free */
	struct vm_proc_pair pair[VM_PROC_MAX]; /* lines 2 to 5 */
	/* cold: written at registration, never read by the fast path */
	struct cap_captbl  *ref[VM_PROC_MAX];  /* lines 6, 7: the reference each entry holds (A65) */
	pgtbl_t             ept;               /* line 8: the VM's page table, recorded at activation */
	struct vm_proc_ctr  ctr;               /* line 9 */
} __attribute__((aligned(CACHELINE_SIZE)));

_Static_assert(__builtin_offsetof(struct vm_proc_tbl, root) == 0, "vm_proc_tbl: roots on line 0");
_Static_assert(__builtin_offsetof(struct vm_proc_tbl, pair) == 2 * CACHELINE_SIZE, "vm_proc_tbl: pairs on line 2");
_Static_assert(sizeof(struct vm_proc_pair) == 16, "vm_proc_tbl: four pairs per line");
_Static_assert(__builtin_offsetof(struct vm_proc_tbl, ref) == 6 * CACHELINE_SIZE, "vm_proc_tbl: cold fields from line 6");
_Static_assert(__builtin_offsetof(struct vm_proc_tbl, ept) == 8 * CACHELINE_SIZE, "vm_proc_tbl: the record on line 8");
_Static_assert(__builtin_offsetof(struct vm_proc_tbl, ctr) == 9 * CACHELINE_SIZE, "vm_proc_tbl: counters on a line of their own");
_Static_assert(sizeof(struct vm_proc_tbl) <= PAGE_SIZE, "vm_proc_tbl: one page");

struct cap_vm_proc_tbl {
	struct cap_header   h;
	struct vm_proc_tbl *tbl;
} __attribute__((packed));

/* The fast-path lookup: no printk; root is already masked. The pair's captbl, or NULL. */
static inline struct captbl *
vm_proc_lkup(struct vm_proc_tbl *t, unsigned long root)
{
	int i;

	if (unlikely(!root)) return NULL;
	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->root[i]) == root) {
			VM_PROC_BARRIER();
			return VM_PROC_ACCESS(t->pair[i].ct);
		}
	}
	return NULL;
}

#if VMCALL_GATE
/* The fast-path lookup under the gate: the root's captbl, only when `rip` is its gate's vmcall. */
static inline struct captbl *
vm_proc_lkup_gate(struct vm_proc_tbl *t, unsigned long root, unsigned long rip)
{
	int i;

	if (unlikely(!root)) return NULL;
	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->root[i]) == root) {
			VM_PROC_BARRIER();
			if (unlikely(VM_PROC_ACCESS(t->pair[i].gate) != rip || !rip)) return NULL;
			return VM_PROC_ACCESS(t->pair[i].ct);
		}
	}
	return NULL;
}
#endif

/* CAPTBL_OP_VM_PROC_TBL_ACTIVATE: `page` is freshly retyped kernel memory; the VM is `vm_comp_cap`. */
static inline int
vm_proc_tbl_activate(struct captbl *t, capid_t cap, capid_t capin, vaddr_t page, capid_t vm_comp_cap)
{
	struct cap_comp        *vmc = (struct cap_comp *)captbl_lkup(t, vm_comp_cap);
	struct cap_vm_proc_tbl *tc;
	struct vm_proc_tbl     *tbl = (struct vm_proc_tbl *)page;
	int                     ret = 0;

	if (unlikely(!CAP_TYPECHK(vmc, CAP_COMP) || !page)) return -EINVAL;
	tc = (struct cap_vm_proc_tbl *)__cap_capactivate_pre(t, cap, capin, CAP_VM_PROC_TBL, &ret);
	if (!tc) return ret;
	memset(tbl, 0, sizeof(struct vm_proc_tbl));
	tbl->ept = vmc->info.pgtblinfo.pgtbl;
	tc->tbl  = tbl;
	__cap_capactivate_post(&tc->h, CAP_VM_PROC_TBL);

	return 0;
}

/*
 * CAPTBL_OP_VM_PROC_ATTACH: point a vCPU thread at its VM's table. The thread must be a VM thread
 * of the table's VM (its first component runs on the table's EPT). Two vCPUs of one VM attach the
 * same table. Done once, before the vCPU first runs (capmgr_vm_proc_attach).
 */
static inline int
vm_proc_attach(struct captbl *t, capid_t thd_cap, capid_t tbl_cap)
{
	struct cap_thd         *thc = (struct cap_thd *)captbl_lkup(t, thd_cap);
	struct cap_vm_proc_tbl *tc  = (struct cap_vm_proc_tbl *)captbl_lkup(t, tbl_cap);
	struct thread          *thd;

	if (unlikely(!CAP_TYPECHK(thc, CAP_THD) || !CAP_TYPECHK(tc, CAP_VM_PROC_TBL))) return -EINVAL;
	thd = thc->t;
	if (unlikely(!thd || thd->thd_type != THD_TYPE_VM)) return -EINVAL;
	if (unlikely(thd->invstk[0].comp_info.pgtblinfo.pgtbl != tc->tbl->ept)) return -EINVAL;
	VM_PROC_ACCESS(thd->vcpu_ctx.proc_tbl) = tc->tbl;

	return 0;
}

/* A65: the reference comp_activate takes on a component's top capability table. */
static inline int
__vm_proc_ref_take(struct cap_captbl *ctc)
{
	u32_t v = ctc->refcnt_flags;

	if (unlikely(v & CAP_MEM_FROZEN_FLAG)) return -EINVAL;
	if (unlikely((v & CAP_REFCNT_MAX) == CAP_REFCNT_MAX)) return -EINVAL;
	if (cos_cas_32((u32_t *)&ctc->refcnt_flags, v, v + 1) != CAS_SUCCESS) return -ECASFAIL;

	return 0;
}

static inline void
__vm_proc_ref_release(struct cap_captbl *ctc)
{
	cos_faa((int *)&ctc->refcnt_flags, -1);
}

/* CAPTBL_OP_VM_PROC_REGISTER, invoked on the table's capability `tc`. */
static inline int
vm_proc_register(struct captbl *ct, struct cap_vm_proc_tbl *tc, capid_t proc_comp_cap, unsigned long root,
                 unsigned long gate)
{
	struct vm_proc_tbl *t = tc->tbl;
	struct cap_comp    *pc;
	int                 i, ret;

	if (unlikely(!root || (root & ~VM_PROC_ROOT_MASK))) return -EINVAL;
	pc = (struct cap_comp *)captbl_lkup(ct, proc_comp_cap);
	if (unlikely(!CAP_TYPECHK(pc, CAP_COMP))) return -EINVAL;
	/* A process component runs on its VM's page table (the EPT), nothing else. */
	if (unlikely(pc->info.pgtblinfo.pgtbl != t->ept)) return -EINVAL;

	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->root[i]) == root) return -EEXIST;
	}
	if ((ret = __vm_proc_ref_take(pc->ct_top))) return ret;
	for (i = 0; i < VM_PROC_MAX; i++) {
		if (VM_PROC_ACCESS(t->root[i]) != 0) continue;
		if (cos_cas(&t->root[i], 0, VM_PROC_ROOT_CLAIMED) != CAS_SUCCESS) continue;
		t->ref[i]     = pc->ct_top;
		t->pair[i].ct = pc->info.captbl;
		t->pair[i].gate = gate; /* before the root (G3's order) */
		VM_PROC_BARRIER();
		VM_PROC_ACCESS(t->root[i]) = root;

		return 0;
	}
	__vm_proc_ref_release(pc->ct_top);

	return -ENOMEM; /* the table is full (the kernel has no ENOSPC) */
}

/* CAPTBL_OP_VM_PROC_UNREGISTER, invoked on the table's capability `tc`. */
static inline int
vm_proc_unregister(struct cap_vm_proc_tbl *tc, unsigned long root)
{
	struct vm_proc_tbl *t = tc->tbl;
	int                 i;

	if (unlikely(!root || (root & ~VM_PROC_ROOT_MASK))) return -EINVAL;
	for (i = 0; i < VM_PROC_MAX; i++) {
		struct cap_captbl *ref;

		if (VM_PROC_ACCESS(t->root[i]) != root) continue;
		/* Stable while the root is set: only a claimed (root 0 -> CLAIMED) entry is rewritten. */
		ref = VM_PROC_ACCESS(t->ref[i]);
		/* The root goes first: from here no fast path can match the entry. */
		if (cos_cas(&t->root[i], root, 0) != CAS_SUCCESS) continue;
		/*
		 * A fast path that read the root before the clear may still use pair[i].ct; the table it
		 * names is freed only after a freeze and its quiescence period (captbl_deactivate).
		 */
		__vm_proc_ref_release(ref);

		return 0;
	}

	return -ENOENT;
}

#endif /* VMCALL_IDENTITY */

#endif /* VM_PROC_H */
