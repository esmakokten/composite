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
/*
 * Phase 3 (VMCALL_GATE). REGISTER_GATE: as REGISTER_LOCKED (rbx root, rcx second root or 0), but rdx
 * is the guest-physical address of one struct errand_id_gate_page, which adds the gate. The VMM
 * locks the pages, walks the tables (A31), checks the gate's mapping, writes the stub into the
 * gate page, makes it read+fetch, then registers with the stub's vmcall as the only served RIP.
 */
#define ERRAND_ID_OP_REGISTER_GATE   0xE106UL

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
#define ERRAND_ID_LOCK_DROP_ONE  0x2UL /* test (3.2): leave the last lower table out of the list */

#define ERRAND_ID_IOC_REGISTER_GATE 0xE1D0000AUL /* arg: struct errand_id_gate_req * */
#define ERRAND_ID_IOC_T5            0xE1D0000BUL /* arg: struct errand_id_t5 *: return to user mode there */
#define ERRAND_ID_IOC_T6            0xE1D0000CUL /* write one byte to the gate page through the direct map */

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

/*
 * Phase 3: the gate registration page. The gate is one guest page the module allocated and mapped
 * into the process at gate_va, read and execute only (DECISIONS A32). comp picks the process
 * component (0: slots 0 and 1; 1: slot 0 only; 2: slot 1 only; A36, a test convenience).
 */
#define ERRAND_ID_GATE_MAX 508
struct errand_id_gate_page {
	unsigned long count;
	unsigned long comp;
	unsigned long gate_va;
	unsigned long gate_gpa;
	unsigned long gpa[ERRAND_ID_GATE_MAX];
};

/*
 * The stub the VMM writes at the start of the gate page (A34); the rest of the page is 0xCC. The
 * caller sets rax = (slot + 1) << 16, rcx = 0 and the arguments in rbx, rsi, rdi, rdx as for an
 * inline vmcall, and calls gate_va; results come back in the same registers.
 *   push %rbp; mov $0x3202,%r11d; vmcall; pop %rbp; ret
 */
#define ERRAND_ID_GATE_STUB { 0x55, 0x41, 0xbb, 0x02, 0x32, 0x00, 0x00, 0x0f, 0x01, 0xc1, 0x5d, 0xc3 }
#define ERRAND_ID_GATE_STUB_LEN    12
#define ERRAND_ID_GATE_VMCALL_OFF  7

struct errand_id_gate_req {
	unsigned long gate_va; /* in: where the process mapped /dev/errand_id (one page) */
	unsigned long comp;    /* in */
	unsigned long flags;   /* in: ERRAND_ID_LOCK_* */
};

/* T5: the module rewrites the calling thread's saved user frame; the ioctl "returns" there. */
struct errand_id_t5 {
	unsigned long rip, rsp, rax, rbx, rcx, rdx, rsi, rdi;
};

/* What the module measured for the last locked registration (T9). */
struct errand_id_reg_stat {
	unsigned long pages;  /* page-table pages sent */
	unsigned long cycles; /* TSC from just before the vmcall to just after it */
	long          ret;    /* the VMM's answer */
};

#endif /* ERRAND_ID_ABI_H */
