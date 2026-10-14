/*
 * libcrypto's heap and initialisation inside a component (cos_openssl.h).
 *
 * The arena: power-of-two blocks of 32 B to 128 KB carved from a static array, one free list
 * per size, a 16-byte header holding the size class so that payloads stay 16-byte aligned.
 * Measured with the guest's libcrypto (2026-10-14): ~200 KB in ~6,000 allocations, none over
 * 8 KB, to initialise and fetch AES-GCM; 3 allocations (~1.1 KB) per EVP_CIPHER_CTX; none per
 * record. Freed blocks are reused, so SETKEY/DELKEY churn does not grow it. Not thread-safe: a
 * component's libcrypto is only entered from one thread at a time (as tlskey's key slots are).
 */
#include <string.h>
#include <openssl/crypto.h>
#include <cos_openssl.h>

#define ARENA_SIZE (4UL * 1024 * 1024)
#define MIN_CLASS  5  /* 32 B */
#define MAX_CLASS  17 /* 128 KB */
#define HDR        16
#define MAGIC      0xc05a11c0U

struct blk {
	unsigned int cls;
	unsigned int magic;
	struct blk  *next; /* on a free list only; the payload starts after the header */
};

static unsigned char arena[ARENA_SIZE] __attribute__((aligned(4096)));
static unsigned long arena_top, arena_used, arena_peak;
static struct blk   *free_list[MAX_CLASS + 1];

static void *
cos_malloc(size_t sz, const char *file, int line)
{
	unsigned int c = MIN_CLASS;
	struct blk  *b;

	if (sz > (1UL << MAX_CLASS) - HDR) return NULL;
	while ((1UL << c) < sz + HDR) c++;
	if ((b = free_list[c])) {
		free_list[c] = b->next;
	} else {
		if (arena_top + (1UL << c) > ARENA_SIZE) return NULL;
		b = (struct blk *)(arena + arena_top);
		arena_top += 1UL << c;
		b->cls   = c;
		b->magic = MAGIC;
	}
	arena_used += 1UL << c;
	if (arena_used > arena_peak) arena_peak = arena_used;

	return (unsigned char *)b + HDR;
}

static void
cos_free(void *p, const char *file, int line)
{
	struct blk *b;

	if (!p) return;
	b = (struct blk *)((unsigned char *)p - HDR);
	if (b->magic != MAGIC || b->cls < MIN_CLASS || b->cls > MAX_CLASS) return; /* not ours: leak, never corrupt */
	arena_used -= 1UL << b->cls;
	b->next = free_list[b->cls];
	free_list[b->cls] = b;
}

static void *
cos_realloc(void *p, size_t sz, const char *file, int line)
{
	struct blk *b;
	void *n;

	if (!p) return cos_malloc(sz, file, line);
	if (!sz) { cos_free(p, file, line); return NULL; }
	b = (struct blk *)((unsigned char *)p - HDR);
	if (sz + HDR <= (1UL << b->cls)) return p;
	if (!(n = cos_malloc(sz, file, line))) return NULL;
	memcpy(n, p, (1UL << b->cls) - HDR);
	cos_free(p, file, line);

	return n;
}

int
cos_openssl_init(void)
{
	if (!CRYPTO_set_mem_functions(cos_malloc, cos_realloc, cos_free)) return -1;
	/* runs OPENSSL_cpuid_setup; getenv("OPENSSL_ia32cap") finds nothing in a component's environment */
	if (!OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT | OPENSSL_INIT_NO_LOAD_CONFIG, NULL)) return -1;

	return 0;
}

extern unsigned int OPENSSL_ia32cap_P[4];

void
cos_openssl_caps(struct cos_openssl_caps *c)
{
	c->aesni  = !!(OPENSSL_ia32cap_P[1] & (1U << (57 - 32)));
	c->pclmul = !!(OPENSSL_ia32cap_P[1] & (1U << (33 - 32)));
	c->avx    = !!(OPENSSL_ia32cap_P[1] & (1U << (60 - 32)));
	c->movbe  = !!(OPENSSL_ia32cap_P[1] & (1U << (54 - 32)));
}

void
cos_openssl_heap(unsigned long *used, unsigned long *peak, unsigned long *size)
{
	*used = arena_used;
	*peak = arena_peak;
	*size = ARENA_SIZE;
}
