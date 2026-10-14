/*
 * The tree's musl sends every system call through __cos_syscall (arch/x86_64/syscall_arch.h).
 * For the host-side check only: issue the real Linux system call, so the component-built
 * libcrypto.a and the tree's libc.a run natively, unchanged.
 */
long
__cos_syscall(int n, long a1, long a2, long a3, long a4, long a5, long a6, long unused)
{
	register long r10 asm("r10") = a4, r8 asm("r8") = a5, r9 asm("r9") = a6;
	long ret;

	asm volatile("syscall" : "=a"(ret) : "a"((long)n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
		     : "rcx", "r11", "memory");

	return ret;
}
