/*
 * errand_id_abi.h -- the one ABI between the guest's errand_id module, its user programs and
 * the VMM (w11_vmcall_identity). Written once here; cos-vmimg carries a byte-identical copy
 * (programs/errand_id_abi.h), compared before every image build. No includes: it is read by
 * the VMM (musl), the Linux module and busybox programs alike.
 *
 * Ring-0 vmcall (the control plane): rax = an ERRAND_ID_OP_*, rbx = its argument. A vmcall
 * from guest ring 0 takes the slow path to the VMM (vmx_exit_asm.S under VMCALL_IDENTITY);
 * the VMM answers in rax (0 or a negative Linux errno) and resumes at RIP + 3. Every op is
 * below 1 << 16, so (rax >> 16) - 1 is never an errand slot.
 */
#ifndef ERRAND_ID_ABI_H
#define ERRAND_ID_ABI_H

#define ERRAND_ID_OP_REGISTER   0xE101UL /* rbx = the exact guest-physical root (DECISIONS A8) */
#define ERRAND_ID_OP_UNREGISTER 0xE102UL /* rbx = a root registered before */
#define ERRAND_ID_OP_COUNTERS   0xE103UL /* the VMM prints its identity counters once */
/*
 * Phase 2 (VMCALL_PT_LOCK). REGISTER_LOCKED: rbx = root, rcx = second root (PTI's user root) or 0,
 * rdx = guest-physical address of one struct errand_id_lock_page. The VMM checks the list, makes
 * every page read-only in the EPT, then registers; the first write to one of them revokes. DUMP:
 * the VMM prints its lock counters once.
 */
#define ERRAND_ID_OP_REGISTER_LOCKED 0xE104UL
#define ERRAND_ID_OP_DUMP            0xE105UL

/* Negative errnos in rax, Linux numbering (Composite's kernel uses the same for these). */
#define ERRAND_ID_ENOENT 2
#define ERRAND_ID_ENOMEM 12
#define ERRAND_ID_EEXIST 17
#define ERRAND_ID_EINVAL 22
#define ERRAND_ID_ENOSYS 38

/*
 * ioctls on /dev/errand_id. Plain numbers, not _IO(): the VMM has no <linux/ioctl.h>, and the
 * module checks them itself.
 */
#define ERRAND_ID_IOC_REGISTER      0xE1D00001UL /* register current's root(s); returns 0 or -errno */
#define ERRAND_ID_IOC_UNREGISTER    0xE1D00002UL /* unregister them */
#define ERRAND_ID_IOC_ROOT          0xE1D00003UL /* arg: unsigned long * <- __pa(current->mm->pgd) */
#define ERRAND_ID_IOC_KERNEL_VMCALL 0xE1D00004UL /* arg: struct errand_id_kvmcall * (T2) */
#define ERRAND_ID_IOC_COUNTERS      0xE1D00005UL /* ring-0 ERRAND_ID_OP_COUNTERS */
#define ERRAND_ID_IOC_REGISTER_LOCKED 0xE1D00006UL /* arg: ERRAND_ID_LOCK_* flags; returns 0 or -errno */
#define ERRAND_ID_IOC_DUMP            0xE1D00007UL /* ring-0 ERRAND_ID_OP_DUMP */
#define ERRAND_ID_IOC_T4_REWRITE      0xE1D00008UL /* arg: a user address in a code page of current (T4) */
#define ERRAND_ID_IOC_LAST_REG        0xE1D00009UL /* arg: struct errand_id_reg_stat * <- the last locked registration */

#define ERRAND_ID_LOCK_NO_PRESET 0x1UL /* do not pre-set accessed and dirty (T3 arm B) */

/* T2's ring-0 attempts: `count` vmcalls from the module with rax = `rax`. */
struct errand_id_kvmcall {
	unsigned long rax;      /* in: e.g. an errand slot, (64 + 4s + 1) << 16 */
	unsigned long count;    /* in */
	unsigned long n_enosys; /* out: calls answered -ENOSYS (refused by the VMM) */
	unsigned long n_other;  /* out: anything else (each one is a failure of T2) */
	unsigned long last_rax; /* out: the last answer */
};

/*
 * The registration page: one 4 KiB guest page, so at most ERRAND_ID_LOCK_MAX page-table pages
 * (DECISIONS A16). Every entry is the guest-physical address of one page-table page of the
 * process, the PGD page(s) included.
 */
#define ERRAND_ID_LOCK_MAX 510
struct errand_id_lock_page {
	unsigned long count;
	unsigned long rsvd;
	unsigned long gpa[ERRAND_ID_LOCK_MAX];
};

/* What the module measured for the last locked registration (T9). */
struct errand_id_reg_stat {
	unsigned long pages;  /* page-table pages sent */
	unsigned long cycles; /* TSC from just before the vmcall to just after it */
	long          ret;    /* the VMM's answer */
};

#endif /* ERRAND_ID_ABI_H */
