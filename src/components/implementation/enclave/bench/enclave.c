/*
 * enclave: the system-call bounce (Gabe's sketch of 2026-10-23, built as a microbenchmark).
 *
 * A program (programs.c, P0-P2) runs inside this component on its own static stack. Its
 * system calls go through enc_syscall: write the request into page 0 of the shared buffer,
 * save the program's context, and switch to the context of the errand that is running it, so
 * that errand returns to the guest with ENC_SYSCALL. The guest performs the call, writes
 * page 1 and re-invokes with RESUME; that errand saves its own context and switches back to
 * the program, which checks page 1's sequence number and returns ret. One errand per call.
 *
 * The invariants from CLAUDE.md, written beside the code that upholds them:
 *   1. The ABI byte for byte: enclave_op is a plain cos_asm_stub entry, four words in, one out.
 *   2. Only rax crosses the errand: every record field is a word in the shared buffer, every
 *      buffer an offset into it.
 *   3. The program never runs on the stub stack (prog_stack, enc_switch_to below).
 *   4. The response page is untrusted data: its seq is checked, ret taken as written.
 *   5. No vector registers: -mgeneral-regs-only (Makefile); objdump checks it.
 *   6. Nothing is printed inside a timed window: the programs print nothing.
 *
 * One program thread and one guest vCPU (DECISIONS.md §4: more threads, teardown and
 * argument sanitising are the design note's, not this prototype's).
 */
#include <cos_component.h>
#include <llprint.h>
#include <string.h>
#include <memmgr.h>
#include <enclave.h>

/* programs.c */
extern long enc_prog_entry(long prog, long n);
extern void enc_prog_result(struct enc_result *r);
extern unsigned long long *enc_prog_samples(unsigned long *n);

/* The shared buffer: this component's mapping, its guest-physical address, its length. */
unsigned char *enc_buf;
static word_t  buf_len;
static word_t  buf_gpa;
static word_t  nerrands;

static inline int
in_buf(word_t off, word_t len)
{
	return enc_buf && off <= buf_len && len <= buf_len - off;
}

/*
 * A continuation: a saved stack pointer and the address to resume at. One for the program,
 * one for the errand currently running it (the caller); both static, as in the sketch.
 */
struct cont {
	word_t sp, ip;
};
static struct cont prog_ctx, caller_ctx;

/*
 * enc_switch_to(save, load): push the callee-saved registers, store rsp and the resume address
 * in *save, load rsp from *load and jump to its address. A context saved here resumes at 1:,
 * pops what it pushed and returns from its own enc_switch_to; a fresh program context resumes
 * at enc_prog_start instead. Everything else is caller-saved by the C ABI, and no vector
 * register is live anywhere in this component (invariant 5).
 */
void enc_switch_to(struct cont *save, struct cont *load);
void enc_prog_start(void);
__asm__(".text\n"
	".globl enc_switch_to\n"
	".type enc_switch_to, @function\n"
	".align 16\n"
	"enc_switch_to:\n\t"
	"pushq %rbp\n\t"
	"pushq %rbx\n\t"
	"pushq %r12\n\t"
	"pushq %r13\n\t"
	"pushq %r14\n\t"
	"pushq %r15\n\t"
	"movq %rsp, 0(%rdi)\n\t"
	"leaq 1f(%rip), %rax\n\t"
	"movq %rax, 8(%rdi)\n\t"
	"movq 0(%rsi), %rsp\n\t"
	"jmpq *8(%rsi)\n"
	"1:\n\t"
	"popq %r15\n\t"
	"popq %r14\n\t"
	"popq %r13\n\t"
	"popq %r12\n\t"
	"popq %rbx\n\t"
	"popq %rbp\n\t"
	"ret\n"
	/* A fresh program's first instruction: rsp is the 16-byte-aligned stack top, so the call
	 * leaves the ABI's entry alignment. enc_prog_main never returns. */
	".globl enc_prog_start\n"
	".type enc_prog_start, @function\n"
	".align 16\n"
	"enc_prog_start:\n\t"
	"xorq %rbp, %rbp\n\t"
	"call enc_prog_main\n\t"
	"ud2\n");

/*
 * Invariant 3. The program's stack: 256 KiB, static (DECISIONS.md Q2). The per-thread stub
 * stack (8 KiB) is discarded at every sret, so a program that returns to the guest mid-call
 * cannot live on it; enclave_op runs on the stub stack and only switches.
 *
 * Composite reads the thread id, core id and invocation token from the top of the current
 * 8 KiB-aligned stack chunk (get_stk_data, offsets 1-4 words). The program's stack is aligned
 * to that size and RUN copies those four words from the stub stack into the top of it, so
 * cos_thdid() and friends stay right while the program uses less than 8 KiB - ENC_STK_RSV of
 * stack (it uses a few hundred bytes). They are copied once per RUN: one thread runs programs.
 */
#define ENC_STK_SZ  (256 * 1024)
#define ENC_STK_RSV 64
static unsigned char prog_stack[ENC_STK_SZ] __attribute__((aligned(COS_STACK_SZ)));

enum { PROG_IDLE, PROG_RUNNING, PROG_WAITING };
static int           prog_state = PROG_IDLE;
static long          prog_status;  /* what RUN or RESUME returns when the program switches back */
static long          run_prog, run_n;
static unsigned long seq;
static long          nseq_bad, nbounces;

/* The program's body, on prog_stack. Finishes by switching back for good. */
void enc_prog_main(void) __attribute__((noreturn, used));
void
enc_prog_main(void)
{
	enc_prog_entry(run_prog, run_n);
	prog_state  = PROG_IDLE;
	prog_status = ENC_DONE;
	enc_switch_to(&prog_ctx, &caller_ctx); /* never resumed: RUN builds a fresh context */
	__builtin_unreachable();
}

/*
 * The bounce, called by the program on prog_stack. Writes page 0, returns ENC_SYSCALL from
 * the errand that is running the program, and continues here on the next RESUME.
 */
long
enc_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
	volatile struct enc_req  *q = (volatile struct enc_req *)(enc_buf + ENC_REQ_OFF);
	volatile struct enc_resp *p = (volatile struct enc_resp *)(enc_buf + ENC_RESP_OFF);
	unsigned long s = ++seq;

	q->nr   = nr;
	q->a[0] = a0;
	q->a[1] = a1;
	q->a[2] = a2;
	q->a[3] = a3;
	q->a[4] = a4;
	q->a[5] = a5;
	q->seq  = s; /* last: the record is complete when seq is */
	nbounces++;

	prog_state  = PROG_WAITING;
	prog_status = ENC_SYSCALL;
	enc_switch_to(&prog_ctx, &caller_ctx);
	/* Resumed by RESUME, on that errand's stub stack's behalf; this is prog_stack still. */

	/* Invariant 4: page 1 is the guest's. Its seq must be ours; ret is taken as written. */
	if (p->seq != s) {
		nseq_bad++;
		return -5; /* -EIO: the program's check fails and counts it */
	}

	return p->ret;
}

static long
op_run(word_t prog, word_t n)
{
	word_t top, stub_top;

	if (!enc_buf) return ENC_ENOBUF;
	if (prog_state != PROG_IDLE) return ENC_EBUSY;
	if (prog >= ENC_NPROGS) return ENC_EINVAL;

	/* Seed the program stack's top with the stub stack's thread data (see prog_stack). */
	top      = (word_t)(prog_stack + ENC_STK_SZ);
	stub_top = ((word_t)__builtin_frame_address(0) & ~((word_t)COS_STACK_SZ - 1)) + COS_STACK_SZ;
	memcpy((void *)(top - 4 * sizeof(word_t)), (void *)(stub_top - 4 * sizeof(word_t)), 4 * sizeof(word_t));

	run_prog    = (long)prog;
	run_n       = (long)n;
	seq         = 0;
	nseq_bad    = 0;
	nbounces    = 0;
	prog_ctx.sp = top - ENC_STK_RSV;
	prog_ctx.ip = (word_t)enc_prog_start;
	prog_state  = PROG_RUNNING;
	enc_switch_to(&caller_ctx, &prog_ctx);

	return prog_status;
}

static long
op_resume(void)
{
	if (prog_state != PROG_WAITING) return ENC_ESTATE;
	prog_state = PROG_RUNNING;
	enc_switch_to(&caller_ctx, &prog_ctx);

	return prog_status;
}

static long
op_report(word_t off, word_t first, word_t count)
{
	unsigned long n;
	unsigned long long *s = enc_prog_samples(&n);

	if (!enc_buf) return ENC_ENOBUF;
	if (first > n) return ENC_ERANGE;
	if (count > n - first) count = n - first;
	if (count > buf_len / sizeof(*s) || !in_buf(off, count * sizeof(*s))) return ENC_ERANGE;
	memcpy(enc_buf + off, s + first, count * sizeof(*s));

	return (long)count;
}

static long
op_result(word_t off)
{
	struct enc_result r;

	if (!enc_buf) return ENC_ENOBUF;
	if (!in_buf(off, sizeof(r))) return ENC_ERANGE;
	enc_prog_result(&r);
	r.nseq_bad = nseq_bad;
	r.nbounces = nbounces;
	memcpy(enc_buf + off, &r, sizeof(r));

	return 0;
}

word_t
enclave_op(word_t op, word_t a, word_t b, word_t c)
{
	long ret;

	nerrands++;
	switch (op) {
	case ENC_OP_INFO_GPA: ret = enc_buf ? (long)buf_gpa : ENC_ENOBUF; break;
	case ENC_OP_INFO_LEN: ret = enc_buf ? (long)buf_len : ENC_ENOBUF; break;
	case ENC_OP_COUNTERS: ret = (long)nerrands; break;
	case ENC_OP_RUN:      ret = op_run(a, b); break;
	case ENC_OP_RESUME:   ret = op_resume(); break;
	case ENC_OP_REPORT:   ret = op_report(a, b, c); break;
	case ENC_OP_RESULT:   ret = op_result(a); break;
	default:              ret = ENC_EINVAL; break;
	}

	return (word_t)ret;
}

word_t
enclave_attach(word_t shm_id, word_t gpa, word_t len, word_t unused)
{
	vaddr_t addr = 0;
	unsigned long npages;

	if (enc_buf) return (word_t)ENC_EBUSY;
	npages = memmgr_shared_page_map_aligned((cbuf_t)shm_id, PAGE_SIZE_4K, &addr);
	if (npages == 0 || addr == 0 || npages * PAGE_SIZE_4K < len || len < ENC_REPORT_OFF + ENC_REPORT_MAX * 8) {
		printc("enclave: attach of shm %lu failed (%lu pages at %lx for %lu bytes)\n", shm_id, npages, addr, len);
		return (word_t)ENC_EINVAL;
	}
	buf_len = len;
	buf_gpa = gpa;
	enc_buf = (unsigned char *)addr;
	printc("enclave: buffer attached: %lu KB, vaddr %p, guest-physical %lx\n", len / 1024, enc_buf, gpa);

	return 0;
}

void
cos_init(void)
{
	printc("enclave: component %ld, program stack %d KB at %p, %d samples\n", cos_compid(),
	       ENC_STK_SZ / 1024, prog_stack, ENC_MAX_SAMPLES);
}

/* A server component with no main must still say so; nothing to run after initialisation. */
void
parallel_main(coreid_t cid)
{
}
