/*
 * w11 Phase 2: the VMM's page-table lock (VMCALL_PT_LOCK).
 *
 * A locked registration (ERRAND_ID_OP_REGISTER_LOCKED) brings the guest-physical addresses of every
 * page-table page of the process. The VMM checks them, makes each read-only in the EPT
 * (memmgr_vm_page_perm), and only then registers the root(s): lock first, register second
 * (invariant 4). The first write to a locked page is an EPT violation that lands here:
 * classified by the exit qualification (SDM Vol. 3C Table 27-7: bit 7 = linear address valid; with
 * it, bit 8 clear = a paging-structure access by the CPU, bit 8 set = guest software), counted,
 * then the process is revoked: unregistered first, its pages writable again, and the write
 * re-executed (RIP is not advanced; the VMM cannot inject a fault, D3). An explicit unregister
 * also unlocks, after the unregister. With VMCALL_PT_LOCK_COUNT_ONLY, a violation unlocks only that
 * page and keeps the registration (T3 arm B).
 *
 * Boot-time unit check (2.2, 2.3): GPA 0x1fff000, the last page of Linux 5.15.107's .bss
 * (__bss_stop = 0xffffffff82000000 in every recipe), is read-only before the guest runs; its first
 * write is reported once and the page restored.
 *
 * Counters are printed by ERRAND_ID_OP_DUMP and mirrored for the kernel's heartbeat; nothing here
 * runs on the fast path.
 */
#include <cos_types.h>
#include <llprint.h>
#include <memmgr.h>
#include <capmgr.h>
#include <vmrt.h>
#include "errand_id_abi.h"
#include "pt_lock.h"

#if VMCALL_PT_LOCK

#define PT_LOCK_UNIT_GPA 0x1fff000UL
#define PL_PROCS         4
#define PL_RIPS          8
#define PL_UNLOCKED      1UL /* low bit of a list entry: count-only mode unlocked it */

struct pl_proc {
	int           used;
	unsigned long root, root2, n;
	unsigned long gpa[ERRAND_ID_LOCK_MAX];
};

static struct pl_proc pl_procs[PL_PROCS];
static compid_t       pl_vm, pl_proc_comp;
static int            pl_unit_pending;

static struct {
	unsigned long reg_ok, reg_fail, unreg_unlock, revoked, locked_now;
	unsigned long viol_hw, viol_sw, viol_nolin, count_only_unlocks;
	unsigned long n_rip;
	struct { unsigned long rip, gpa, qual; } rip[PL_RIPS];
} pl;

static void
pl_hb(struct vmrt_vm_vcpu *vcpu)
{
#if VMM_HEARTBEAT
	struct vm_hb_ctr *k = VM_HB_CTR(vcpu->shared_region);

	k->pl_locked  = pl.locked_now;
	k->pl_revoked = pl.revoked;
	k->pl_viol_hw = pl.viol_hw;
	k->pl_viol_sw = pl.viol_sw;
#endif
}

void
pt_lock_init(compid_t vm, compid_t proc_comp)
{
	int ret;

	pl_vm        = vm;
	pl_proc_comp = proc_comp;
	ret          = memmgr_vm_page_perm(vm, PT_LOCK_UNIT_GPA, 1, VM_EPT_PERM_RO);
	printc("VMM: pt_lock unit check: gpa %lx read-only before the guest runs: %d\n", PT_LOCK_UNIT_GPA, ret);
	pl_unit_pending = (ret == 0);
}

static int
pl_gpa_ok(struct vmrt_vm_vcpu *vcpu, unsigned long gpa)
{
	return !(gpa & (PAGE_SIZE_4K - 1)) && gpa >= PAGE_SIZE_4K
	       && gpa - PAGE_SIZE_4K < vcpu->vm->guest_mem_sz;
}

/* Make `p`'s still-locked pages writable again. */
static void
pl_unlock_all(struct pl_proc *p)
{
	unsigned long i;

	for (i = 0; i < p->n; i++) {
		if (p->gpa[i] & PL_UNLOCKED) continue;
		if (memmgr_vm_page_perm(pl_vm, p->gpa[i], 1, VM_EPT_PERM_DEF) == 0) pl.locked_now--;
		p->gpa[i] |= PL_UNLOCKED;
	}
}

long
pt_lock_register(struct vmrt_vm_vcpu *vcpu, unsigned long root, unsigned long root2, unsigned long list_gpa)
{
	struct errand_id_lock_page *lp;
	struct pl_proc             *p = NULL;
	unsigned long               i = 0, j, n = 0;
	int                         r1 = 0, r2 = !root2;
	long                        ret;
	const char                 *why = "addresses";

	if (!pl_gpa_ok(vcpu, list_gpa) || !pl_gpa_ok(vcpu, root) || (root2 && !pl_gpa_ok(vcpu, root2))) goto inval;
	for (i = 0; i < PL_PROCS; i++) {
		if (!pl_procs[i].used) { p = &pl_procs[i]; break; }
	}
	if (!p) { ret = -ERRAND_ID_ENOMEM; goto fail; }

	/* Copy the list out of guest memory before checking it: the guest could change it after. */
	lp = (struct errand_id_lock_page *)GPA2HVA(list_gpa, vcpu->vm);
	n  = lp->count;
	why = "count";
	if (n == 0 || n > ERRAND_ID_LOCK_MAX) goto inval;
	for (i = 0; i < n; i++) p->gpa[i] = lp->gpa[i];
	for (i = 0; i < n; i++) {
		why = "a page outside guest memory";
		if (!pl_gpa_ok(vcpu, p->gpa[i])) goto inval;
		if (p->gpa[i] == root) r1 = 1;
		if (root2 && p->gpa[i] == root2) r2 = 1;
		/* no page twice, and none already locked by another registration */
		why = "a page listed twice";
		for (j = 0; j < i; j++) if (p->gpa[j] == p->gpa[i]) goto inval;
		why = "a page another registration holds";
		for (j = 0; j < PL_PROCS; j++) {
			unsigned long k;

			if (!pl_procs[j].used) continue;
			for (k = 0; k < pl_procs[j].n; k++) {
				if ((pl_procs[j].gpa[k] & ~PL_UNLOCKED) == p->gpa[i]) goto inval;
			}
		}
	}
	why = "a root page missing from the list";
	if (!r1 || !r2) goto inval; /* the root pages must be in the list */
	p->n     = n;
	p->root  = root;
	p->root2 = root2;

	/* Lock first ... */
	for (i = 0; i < n; i++) {
		ret = memmgr_vm_page_perm(pl_vm, p->gpa[i], 1, VM_EPT_PERM_RO);
		if (ret) {
			p->n = i;
			pl_unlock_all(p);
			goto fail;
		}
		pl.locked_now++;
	}
	/* ... register second. */
	ret = capmgr_vm_proc_register(pl_vm, pl_proc_comp, root);
	if (!ret && root2) {
		ret = capmgr_vm_proc_register(pl_vm, pl_proc_comp, root2);
		if (ret) capmgr_vm_proc_unregister(pl_vm, root);
	}
	if (ret) {
		pl_unlock_all(p);
		goto fail;
	}
	p->used = 1;
	pl.reg_ok++;
	pl_hb(vcpu);

	return 0;
inval:
	ret = -ERRAND_ID_EINVAL;
	printc("VMM: pt_lock: locked registration of root %lx refused: %s (entry %lu of %lu: %lx)\n", root, why, i, n,
	       (p && i < n) ? p->gpa[i] : 0UL);
fail:
	pl.reg_fail++;
	return ret;
}

static struct pl_proc *
pl_find_root(unsigned long root)
{
	int i;

	for (i = 0; i < PL_PROCS; i++) {
		if (pl_procs[i].used && (pl_procs[i].root == root || pl_procs[i].root2 == root)) return &pl_procs[i];
	}
	return NULL;
}

void
pt_lock_unregistered(struct vmrt_vm_vcpu *vcpu, unsigned long root)
{
	struct pl_proc *p = pl_find_root(root);

	if (!p) return;
	/* Unregister before unlock (invariant 4): the other root goes too. */
	if (p->root2) capmgr_vm_proc_unregister(pl_vm, p->root == root ? p->root2 : p->root);
	pl_unlock_all(p);
	p->used = 0;
	pl.unreg_unlock++;
	pl_hb(vcpu);
}

int
pt_lock_ept_violation(struct vmrt_vm_vcpu *vcpu)
{
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;
	unsigned long                          gpa  = regs->gpa & ~(PAGE_SIZE_4K - 1);
	unsigned long                          qual = regs->qualification, i;
	struct pl_proc                        *p = NULL;
	int                                    ret, k;

	if (pl_unit_pending && gpa == PT_LOCK_UNIT_GPA) {
		pl_unit_pending = 0;
		ret = memmgr_vm_page_perm(pl_vm, gpa, 1, VM_EPT_PERM_DEF);
		printc("VMM: pt_lock unit check: EPT violation at gpa %lx, qualification %lx, rip %lx; "
		       "page writable again: %d; UNIT CHECK %s\n", (unsigned long)regs->gpa, qual,
		       (unsigned long)regs->ip, ret, ((qual & 0x2) && !ret) ? "PASSED" : "FAILED");
		return 1; /* re-execute the write */
	}

	for (k = 0; k < PL_PROCS && !p; k++) {
		if (!pl_procs[k].used) continue;
		for (i = 0; i < pl_procs[k].n; i++) {
			if (pl_procs[k].gpa[i] == gpa) { p = &pl_procs[k]; break; }
		}
	}
	if (!p) return 0;

	if (!(qual & (1UL << 7)))      pl.viol_nolin++;
	else if (!(qual & (1UL << 8))) pl.viol_hw++;
	else                           pl.viol_sw++;
	if (pl.n_rip < PL_RIPS) {
		pl.rip[pl.n_rip].rip  = regs->ip;
		pl.rip[pl.n_rip].gpa  = regs->gpa;
		pl.rip[pl.n_rip].qual = qual;
	}
	pl.n_rip++;

#if VMCALL_PT_LOCK_COUNT_ONLY
	/* T3 arm B: unlock only this page, keep the registration */
	if (memmgr_vm_page_perm(pl_vm, gpa, 1, VM_EPT_PERM_DEF) == 0) pl.locked_now--;
	p->gpa[i] |= PL_UNLOCKED;
	pl.count_only_unlocks++;
#else
	/* Revoke: unregister first, then make every page of the process writable again. */
	capmgr_vm_proc_unregister(pl_vm, p->root);
	if (p->root2) capmgr_vm_proc_unregister(pl_vm, p->root2);
	pl_unlock_all(p);
	p->used = 0;
	pl.revoked++;
#endif
	pl_hb(vcpu);

	return 1; /* RIP not advanced: the write runs again, now allowed */
}

void
pt_lock_dump(void)
{
	unsigned long i;

	printc("VMM: pt_lock: locked registrations ok %lu fail %lu, unregister+unlock %lu, revoked %lu, "
	       "pages locked now %lu; violations hw %lu sw %lu no-linear %lu; count-only unlocks %lu%s\n",
	       pl.reg_ok, pl.reg_fail, pl.unreg_unlock, pl.revoked, pl.locked_now, pl.viol_hw, pl.viol_sw,
	       pl.viol_nolin, pl.count_only_unlocks, VMCALL_PT_LOCK_COUNT_ONLY ? " (count-only mode)" : "");
	for (i = 0; i < pl.n_rip && i < PL_RIPS; i++) {
		printc("VMM: pt_lock: violation %lu: rip %lx gpa %lx qualification %lx (%s)\n", i, pl.rip[i].rip,
		       pl.rip[i].gpa, pl.rip[i].qual,
		       !(pl.rip[i].qual & 0x80) ? "no linear" : (pl.rip[i].qual & 0x100) ? "software" : "hardware");
	}
}

#endif /* VMCALL_PT_LOCK */
