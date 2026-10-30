/*
 * enclave_progs.h -- the programs P0-P2 and the timing macro, shared byte for byte between the
 * enclave component (src/components/interface/enclave/) and the guest's encbench
 * (cos-vmimg/programs/), so the bounced arm and the native arm run the same source.
 *
 * The includer provides, before including this file:
 *   long enc_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
 *       the component: the bounce (write page 0, return to the guest, resume on page 1);
 *       encbench's native arm: the syscall instruction (ENC_NR_NULL answers 0 without one).
 *   ENC_PTR(off)  an unsigned char * to shared-buffer offset `off` in this address space
 *   ENC_ARG(off)  the system-call argument naming that offset: the component passes the
 *                 offset itself (the guest stub adds its base), the native arm a pointer
 *   enc_samples[ENC_MAX_SAMPLES] (unsigned long long)
 * and includes enclave_abi.h.
 *
 * Every call is timed alone: lfence; rdtsc before, rdtscp; lfence after, the sample is the
 * difference. Each call's ret is checked against what it must return outside the window. The
 * calls a row needs but does not time (the close after an open, the munmap after an mmap,
 * the mmap before a munmap, the close after a socket) are made through enc_syscall too, untimed.
 * Nothing is printed: the programs run inside the component, where a print is a serial exit.
 *
 * No floating point and no vector registers: the component is built -mgeneral-regs-only.
 */
#ifndef ENCLAVE_PROGS_H
#define ENCLAVE_PROGS_H

static inline unsigned long long
enc_t_begin(void)
{
	unsigned int a, d;

	__asm__ __volatile__("lfence\n\trdtsc" : "=a"(a), "=d"(d) : : "memory");

	return ((unsigned long long)d << 32) | a;
}

static inline unsigned long long
enc_t_end(void)
{
	unsigned int a, d, c;

	__asm__ __volatile__("rdtscp\n\tlfence" : "=a"(a), "=d"(d), "=c"(c) : : "memory");

	return ((unsigned long long)d << 32) | a;
}

/* Data-page layout of P2 (offsets into the shared buffer). */
#define ENC_IO_OFF    (ENC_DATA_OFF)            /* the 10 KB read/write buffer */
#define ENC_IO_LEN    10240
#define ENC_PATH_OFF  (ENC_DATA_OFF + 0x4000)   /* "/tmp/bounce.txt", 10 KB, made by the init script */
#define ENC_NULL_OFF  (ENC_DATA_OFF + 0x4040)   /* "/dev/null" */
#define ENC_MSG_OFF   (ENC_DATA_OFF + 0x4080)   /* the 13-byte "printf" */
#define ENC_MSG_LEN   13
#define ENC_MAP_LEN   10240

/* Linux x86_64 flag values (the component has no Linux headers). */
#define ENC_O_RDONLY  0
#define ENC_O_WRONLY  1
#define ENC_O_RDWR    2
#define ENC_PROT_RW   0x3  /* PROT_READ | PROT_WRITE */
#define ENC_MAP_PA    0x22 /* MAP_PRIVATE | MAP_ANONYMOUS */
#define ENC_AF_INET   2
#define ENC_SOCK_STREAM 1

static const char *const enc_row_names[ENC_NPROGS][7] = {
	{ "null" },
	{ "getpid" },
	{ "open", "pread", "pwrite", "mmap", "munmap", "socket", "printf" },
};
static const long enc_prog_rows[ENC_NPROGS]  = { 1, 1, 7 };
static const long enc_prog_defn[ENC_NPROGS]  = { 100000, 100000, 10000 };
/* What sits outside each row's window, printed beside the row. */
static const char *const enc_row_outside[ENC_NPROGS][7] = {
	{ "nothing: the guest answers 0 without a call" },
	{ "nothing" },
	{ "the close of the fd it returns", "the fd, opened once", "the fd, opened once",
	  "the munmap of the region", "the mmap of the region", "the close of the socket",
	  "the /dev/null fd, opened once" },
};

static struct enc_result enc_res;
static unsigned long     enc_nsamp;

#define ENC_SYS0(nr)                   enc_syscall((nr), 0, 0, 0, 0, 0, 0)
#define ENC_SYS1(nr, a)                enc_syscall((nr), (long)(a), 0, 0, 0, 0, 0)
#define ENC_SYS2(nr, a, b)             enc_syscall((nr), (long)(a), (long)(b), 0, 0, 0, 0)
#define ENC_SYS3(nr, a, b, c)          enc_syscall((nr), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define ENC_SYS4(nr, a, b, c, d)       enc_syscall((nr), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define ENC_SYS6(nr, a, b, c, d, e, f) \
	enc_syscall((nr), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* One timed call: the window is exactly the enc_syscall. */
#define ENC_TIMED(ret, call)                                   \
	do {                                                   \
		unsigned long long t0_ = enc_t_begin();        \
		(ret) = (call);                                \
		unsigned long long t1_ = enc_t_end();          \
		if (enc_nsamp < ENC_MAX_SAMPLES) enc_samples[enc_nsamp++] = t1_ - t0_; \
	} while (0)

static inline void
enc_check(long row, long ret, int ok)
{
	if (ok) return;
	if (enc_res.nfail++ == 0) {
		enc_res.fail_row = row;
		enc_res.fail_ret = ret;
	}
}

static void
enc_strput(unsigned long off, const char *s)
{
	unsigned char *d = ENC_PTR(off);

	while ((*d++ = (unsigned char)*s++)) ;
}

static void
enc_p0(long n)
{
	long i, r;

	for (i = 0; i < n; i++) {
		ENC_TIMED(r, ENC_SYS0(ENC_NR_NULL));
		enc_check(0, r, r == 0);
	}
}

static void
enc_p1(long n)
{
	long i, r;

	for (i = 0; i < n; i++) {
		ENC_TIMED(r, ENC_SYS0(ENC_NR_getpid));
		enc_check(0, r, r > 0);
	}
}

/* mmap's failure values are -4095..-1; anything else is an address. */
#define ENC_MMAP_OK(r) ((unsigned long)(r) < (unsigned long)-4095L && ((r) & 0xfff) == 0)

static void
enc_p2(long n)
{
	long i, r, fd, nfd, m;

	enc_strput(ENC_PATH_OFF, "/tmp/bounce.txt");
	enc_strput(ENC_NULL_OFF, "/dev/null");
	enc_strput(ENC_MSG_OFF, "hello, world\n");
	fd  = ENC_SYS2(ENC_NR_open, ENC_ARG(ENC_PATH_OFF), ENC_O_RDWR);
	nfd = ENC_SYS2(ENC_NR_open, ENC_ARG(ENC_NULL_OFF), ENC_O_WRONLY);
	enc_check(-1, fd, fd >= 0);
	enc_check(-1, nfd, nfd >= 0);

	for (i = 0; i < n; i++) { /* row 0: open; its close outside the window */
		ENC_TIMED(r, ENC_SYS2(ENC_NR_open, ENC_ARG(ENC_PATH_OFF), ENC_O_RDONLY));
		enc_check(0, r, r >= 0);
		if (r >= 0) ENC_SYS1(ENC_NR_close, r);
	}
	for (i = 0; i < n; i++) { /* row 1: pread 10 KB at offset 0 */
		ENC_TIMED(r, ENC_SYS4(ENC_NR_pread64, fd, ENC_ARG(ENC_IO_OFF), ENC_IO_LEN, 0));
		enc_check(1, r, r == ENC_IO_LEN);
	}
	for (i = 0; i < n; i++) { /* row 2: pwrite 10 KB at offset 0 */
		ENC_TIMED(r, ENC_SYS4(ENC_NR_pwrite64, fd, ENC_ARG(ENC_IO_OFF), ENC_IO_LEN, 0));
		enc_check(2, r, r == ENC_IO_LEN);
	}
	for (i = 0; i < n; i++) { /* row 3: mmap 10 KB anonymous; its munmap outside the window */
		ENC_TIMED(r, ENC_SYS6(ENC_NR_mmap, 0, ENC_MAP_LEN, ENC_PROT_RW, ENC_MAP_PA, -1, 0));
		enc_check(3, r, ENC_MMAP_OK(r));
		if (ENC_MMAP_OK(r)) ENC_SYS2(ENC_NR_munmap, r, ENC_MAP_LEN);
	}
	for (i = 0; i < n; i++) { /* row 4: munmap of a region mapped outside the window */
		m = ENC_SYS6(ENC_NR_mmap, 0, ENC_MAP_LEN, ENC_PROT_RW, ENC_MAP_PA, -1, 0);
		enc_check(4, m, ENC_MMAP_OK(m));
		ENC_TIMED(r, ENC_SYS2(ENC_NR_munmap, m, ENC_MAP_LEN));
		enc_check(4, r, r == 0);
	}
	for (i = 0; i < n; i++) { /* row 5: socket(AF_INET, SOCK_STREAM); its close outside */
		ENC_TIMED(r, ENC_SYS3(ENC_NR_socket, ENC_AF_INET, ENC_SOCK_STREAM, 0));
		enc_check(5, r, r >= 0);
		if (r >= 0) ENC_SYS1(ENC_NR_close, r);
	}
	for (i = 0; i < n; i++) { /* row 6: "printf" = write of 13 bytes to /dev/null */
		ENC_TIMED(r, ENC_SYS3(ENC_NR_write, nfd, ENC_ARG(ENC_MSG_OFF), ENC_MSG_LEN));
		enc_check(6, r, r == ENC_MSG_LEN);
	}

	if (fd >= 0) ENC_SYS1(ENC_NR_close, fd);
	if (nfd >= 0) ENC_SYS1(ENC_NR_close, nfd);
}

/* Run program `prog` with n calls per row (0: the default); the samples go to enc_samples in
 * row order. Returns 0, or ENC_EINVAL for a bad program or a run that would not fit. */
static long
enc_prog_run(long prog, long n)
{
	if (prog < 0 || prog >= ENC_NPROGS) return ENC_EINVAL;
	if (n <= 0) n = enc_prog_defn[prog];
	if (n * enc_prog_rows[prog] > ENC_MAX_SAMPLES) return ENC_EINVAL;

	enc_res.prog     = prog;
	enc_res.n        = n;
	enc_res.nrows    = enc_prog_rows[prog];
	enc_res.nfail    = 0;
	enc_res.fail_row = 0;
	enc_res.fail_ret = 0;
	enc_nsamp        = 0;

	switch (prog) {
	case ENC_PROG_P0: enc_p0(n); break;
	case ENC_PROG_P1: enc_p1(n); break;
	case ENC_PROG_P2: enc_p2(n); break;
	}
	enc_res.nsamples = (long)enc_nsamp;

	return 0;
}

#endif /* ENCLAVE_PROGS_H */
