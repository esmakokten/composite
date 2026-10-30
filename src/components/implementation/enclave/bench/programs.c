/*
 * programs.c: the component side of the programs P0-P2. The programs themselves are in
 * enclave_progs.h, shared byte for byte with the guest's encbench, whose native arm links the
 * same source with enc_syscall = the syscall instruction. Here enc_syscall is the bounce
 * (enclave.c), and a buffer argument is its offset in the shared buffer: the guest stub adds
 * its own mapping's base (invariant 2: the records carry no pointers).
 *
 * These functions run on the program's stack (enclave.c, prog_stack), never on a stub stack.
 */
#include <cos_component.h>
#include <enclave_abi.h>

extern unsigned char *enc_buf;
extern long enc_syscall(long nr, long a0, long a1, long a2, long a3, long a4, long a5);

#define ENC_PTR(off) (enc_buf + (off))
#define ENC_ARG(off) ((long)(off))

/* Samples stay in component memory until REPORT copies them out (invariant 6). */
static unsigned long long enc_samples[ENC_MAX_SAMPLES];

#include <enclave_progs.h>

long
enc_prog_entry(long prog, long n)
{
	return enc_prog_run(prog, n);
}

void
enc_prog_result(struct enc_result *r)
{
	*r = enc_res;
}

unsigned long long *
enc_prog_samples(unsigned long *n)
{
	*n = enc_nsamp;

	return enc_samples;
}
