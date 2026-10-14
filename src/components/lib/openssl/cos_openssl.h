#ifndef COS_OPENSSL_H
#define COS_OPENSSL_H

/*
 * libcrypto in a component. A component has no heap (its default cos_syscall_handler asserts on
 * the brk/mmap that musl's malloc would issue), so cos_openssl_init hands libcrypto a static
 * arena through CRYPTO_set_mem_functions, then initialises it (CPU capabilities included) with
 * no atexit handler and no config file. Call it once, from cos_init, before any other libcrypto
 * call. Returns 0, or -1 if libcrypto would not initialise.
 */
int cos_openssl_init(void);

/* OPENSSL_ia32cap_P, as libcrypto's own dispatch reads it (valid after cos_openssl_init) */
struct cos_openssl_caps {
	int aesni, pclmul, avx, movbe;
};
void cos_openssl_caps(struct cos_openssl_caps *c);

/* Arena use in bytes: currently allocated (block sizes), the high-water mark, the arena size */
void cos_openssl_heap(unsigned long *used, unsigned long *peak, unsigned long *size);

#endif /* COS_OPENSSL_H */
