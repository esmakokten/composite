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

/* T2's ring-0 attempts: `count` vmcalls from the module with rax = `rax`. */
struct errand_id_kvmcall {
	unsigned long rax;      /* in: e.g. an errand slot, (64 + 4s + 1) << 16 */
	unsigned long count;    /* in */
	unsigned long n_enosys; /* out: calls answered -ENOSYS (refused by the VMM) */
	unsigned long n_other;  /* out: anything else (each one is a failure of T2) */
	unsigned long last_rax; /* out: the last answer */
};

#endif /* ERRAND_ID_ABI_H */
