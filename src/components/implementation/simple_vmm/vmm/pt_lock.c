/*
 * w11 Phase 2: the VMM's page-table lock (VMCALL_PT_LOCK).
 *
 * Boot-time unit check (2.2, 2.3): before the guest runs, one guest page is made read-only in the
 * EPT through memmgr_vm_page_perm; the guest's first write to it is an EPT violation that lands
 * here. The VMM prints it once, makes the page writable again and returns without advancing RIP,
 * so the guest re-executes the write. The page is the last page of Linux 5.15.107's .bss
 * (__bss_stop = 0xffffffff82000000 in every recipe's System.map, so GPA 0x2000000 - 4 KiB), which
 * clear_bss writes early in boot.
 */
#include <cos_types.h>
#include <llprint.h>
#include <memmgr.h>
#include <vmrt.h>
#include "pt_lock.h"

#if VMCALL_PT_LOCK

#define PT_LOCK_UNIT_GPA 0x1fff000UL

static compid_t pl_vm;
static int      pl_unit_pending;

void
pt_lock_init(compid_t vm)
{
	int ret;

	pl_vm = vm;
	ret   = memmgr_vm_page_perm(vm, PT_LOCK_UNIT_GPA, 1, VM_EPT_PERM_RO);
	printc("VMM: pt_lock unit check: gpa %lx read-only before the guest runs: %d\n", PT_LOCK_UNIT_GPA, ret);
	pl_unit_pending = (ret == 0);
}

int
pt_lock_ept_violation(struct vmrt_vm_vcpu *vcpu)
{
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;
	unsigned long                          gpa  = regs->gpa & ~(PAGE_SIZE_4K - 1);
	int                                    ret;

	if (pl_unit_pending && gpa == PT_LOCK_UNIT_GPA) {
		pl_unit_pending = 0;
		ret = memmgr_vm_page_perm(pl_vm, gpa, 1, VM_EPT_PERM_DEF);
		printc("VMM: pt_lock unit check: EPT violation at gpa %lx, qualification %lx, rip %lx; "
		       "page writable again: %d; UNIT CHECK %s\n", (unsigned long)regs->gpa,
		       (unsigned long)regs->qualification, (unsigned long)regs->ip, ret,
		       ((regs->qualification & 0x2) && !ret) ? "PASSED" : "FAILED");
		return 1; /* re-execute the write */
	}

	return 0;
}

#endif /* VMCALL_PT_LOCK */
