/*
 * enclave errand ABI -- shared, byte for byte, between the Composite component
 * (src/components/interface/enclave/enclave_abi.h) and the guest
 * (cos-vmimg/programs/enclave_abi.h). Plain C, no Composite or Linux includes.
 *
 * The system-call bounce (Gabe's sketch of 2026-10-23): a program runs inside the enclave
 * component on its own stack. To make a system call it writes a request record into page 0
 * of the shared buffer, saves its stack pointer and resume address in a continuation record,
 * and switches to the context of the errand that is running it, so that errand *returns* to
 * the guest with ENC_SYSCALL. The guest performs the call, writes page 1, and re-invokes with
 * RESUME; the entry restores the program's stack pointer and jumps to the saved address.
 * One errand round trip per system call.
 *
 * One errand: enclave_op(op, a, b, c) -> one word (only rax comes back from an errand).
 * Guest register ABI (libErrand.h): rax = (64 + 4 * slot + 1) << 16, rbx = op, rsi = a,
 * rdi = b, rdx = c.
 *
 * Records carry offsets into the shared buffer, never pointers: the component and the guest
 * share no address space. The guest stub adds its own mapping's base to the arguments that
 * enc_ptr_args() names. Page 1 is written by the guest and is untrusted: the component checks
 * its sequence number and takes ret as written, nothing else (DECISIONS.md Q4).
 */
#ifndef ENCLAVE_ABI_H
#define ENCLAVE_ABI_H

#define ENC_SLOT 4 /* errand slot; 0 = pong_args, 1 = tlskey, 2 and 3 reserved (w10) */

#define ENC_OP_INFO_GPA 1 /* -> guest-physical address of the shared buffer */
#define ENC_OP_INFO_LEN 2 /* -> its length in bytes */
#define ENC_OP_COUNTERS 3 /* -> errands served since boot (this op included) */
#define ENC_OP_RUN      4 /* (prog, n): start program prog, n calls per row (0: its default) */
#define ENC_OP_RESUME   5 /* (ret): the guest has written page 1; ret is informational only */
#define ENC_OP_REPORT   6 /* (off, first, count): samples [first, first + count) as u64 at off -> copied */
#define ENC_OP_RESULT   7 /* (off): struct enc_result of the last program at off -> 0 */

/* RUN and RESUME return one of these, or a negative ENC_E*. */
#define ENC_SYSCALL 1 /* the program waits on the call in page 0 */
#define ENC_DONE    2 /* the program finished */

#define ENC_EINVAL (-1L) /* bad op or program number */
#define ENC_ERANGE (-2L) /* an (off, len) falls outside the buffer */
#define ENC_ENOBUF (-3L) /* no buffer attached yet */
#define ENC_EBUSY  (-4L) /* RUN while a program is waiting on a call; attach twice */
#define ENC_ESTATE (-5L) /* RESUME with no program waiting */

/* Shared-buffer layout (2 MiB object). */
#define ENC_REQ_OFF    0x0      /* page 0: struct enc_req, written by the component */
#define ENC_RESP_OFF   0x1000   /* page 1: struct enc_resp, written by the guest */
#define ENC_DATA_OFF   0x2000   /* pages 2..: I/O buffers and path strings */
#define ENC_REPORT_OFF 0x100000 /* the second MiB: samples copied out by REPORT */
#define ENC_REPORT_MAX (0x100000 / 8)

struct enc_req {
	unsigned long seq; /* written last */
	long          nr;  /* Linux x86_64 system-call number, or ENC_NR_NULL */
	long          a[6];
};

struct enc_resp {
	unsigned long seq; /* = the request's seq, written last */
	long          ret; /* the raw kernel return value (negative errno on failure) */
};

#define ENC_NR_NULL (-1L) /* the null bounce: the guest answers 0 without a call */

/* Linux x86_64 numbers of the calls the programs make, and the arguments of each that are
 * buffer offsets (bit i = a[i]); the guest stub adds its mapping's base to exactly those. */
#define ENC_NR_read     0
#define ENC_NR_write    1
#define ENC_NR_open     2
#define ENC_NR_close    3
#define ENC_NR_mmap     9
#define ENC_NR_munmap   11
#define ENC_NR_pread64  17
#define ENC_NR_pwrite64 18
#define ENC_NR_getpid   39
#define ENC_NR_socket   41

static inline unsigned int
enc_ptr_args(long nr)
{
	switch (nr) {
	case ENC_NR_open: return 1 << 0;
	case ENC_NR_read: case ENC_NR_write: case ENC_NR_pread64: case ENC_NR_pwrite64: return 1 << 1;
	default: return 0;
	}
}

/* Programs (enclave_progs.h): one row per timed call; every row has n samples. */
#define ENC_PROG_P0 0 /* null bounce, 1 row, default n 100,000 */
#define ENC_PROG_P1 1 /* getpid, 1 row, default n 100,000 */
#define ENC_PROG_P2 2 /* Veil's seven, 7 rows, default n 10,000 */
#define ENC_NPROGS  3
#define ENC_MAX_SAMPLES 100000

struct enc_result {
	long prog, n, nrows, nsamples;
	long nfail, fail_row, fail_ret; /* calls whose ret failed the program's check; the first one */
	long nseq_bad;                  /* bounced arm: responses whose seq did not match */
	long nbounces;                  /* bounced arm: system calls bounced (timed and untimed) */
};

/* The VMM's census-dump port (as TLSKEY_CENSUS_PORT): outb(tag, 0x505) prints its slow exits. */
#define ENC_CENSUS_PORT 0x505

#endif /* ENCLAVE_ABI_H */
