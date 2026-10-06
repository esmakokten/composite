/*
 * tlskey: the TLS key service. A guest process (nginx) reaches tlskey_op through one vmcall
 * errand per TLS record; the key never leaves this component. The three invariants from
 * CLAUDE.md are written beside the code that upholds them:
 *
 *   1. Nothing read from the guest is trusted. Every (off, len) is checked against this
 *      component's own mapping of the buffer, and the part of a request the service
 *      interprets (nonce, aad_len, aad, key bytes, tag) is copied out once before it is used.
 *      The payload is only transformed in place: a guest that rewrites it concurrently gets a
 *      result it could have asked for directly, nothing more.
 *   2. No key material leaves in registers: XMM0-15 are zeroed and MXCSR is restored before
 *      every return from tlskey_op (tlskey_leave). Expanded keys live in this component's
 *      memory, which the guest cannot map.
 *   3. Only rax comes back from an errand. Every op returns one word; data goes through the
 *      buffer.
 */
#include <cos_component.h>
#include <llprint.h>
#include <string.h>
#include <memmgr.h>
#include <tlskey.h>
#include <bearssl.h>

struct tlskey_key {
	int                     used;
	br_aes_x86ni_ctr_keys   aes; /* the vtable pointer is its first member: &aes.vtable is the bctx */
	br_gcm_context          gcm;
};

static struct tlskey_key keys[TLSKEY_MAX_KEYS];

/* The shared buffer: this component's mapping, its guest-physical address, its length. */
static unsigned char *buf;
static word_t         buf_len;
static word_t         buf_gpa;
static word_t         nerrands;

/* Invariant 1: every (off, len) the guest names is checked against our own mapping. */
static inline int
in_buf(word_t off, word_t len)
{
	return buf && off <= buf_len && len <= buf_len - off;
}

/* Invariant 2: scrub XMM0-15 and restore the caller's MXCSR. The last thing before return. */
static inline void
tlskey_leave(unsigned int mxcsr)
{
	asm volatile("pxor %%xmm0, %%xmm0\n\t"   "pxor %%xmm1, %%xmm1\n\t"
		     "pxor %%xmm2, %%xmm2\n\t"   "pxor %%xmm3, %%xmm3\n\t"
		     "pxor %%xmm4, %%xmm4\n\t"   "pxor %%xmm5, %%xmm5\n\t"
		     "pxor %%xmm6, %%xmm6\n\t"   "pxor %%xmm7, %%xmm7\n\t"
		     "pxor %%xmm8, %%xmm8\n\t"   "pxor %%xmm9, %%xmm9\n\t"
		     "pxor %%xmm10, %%xmm10\n\t" "pxor %%xmm11, %%xmm11\n\t"
		     "pxor %%xmm12, %%xmm12\n\t" "pxor %%xmm13, %%xmm13\n\t"
		     "pxor %%xmm14, %%xmm14\n\t" "pxor %%xmm15, %%xmm15\n\t"
		     "ldmxcsr %0"
		     : : "m"(mxcsr)
		     : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
		       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
}

static void
key_init(struct tlskey_key *k, const unsigned char *key, size_t len)
{
	br_aes_x86ni_ctr_init(&k->aes, key, len);
	br_gcm_init(&k->gcm, &k->aes.vtable, br_ghash_pclmul);
}

/* The record header, copied out of the shared buffer once (invariant 1). */
struct tlskey_hdr {
	unsigned char nonce[TLSKEY_NONCE_LEN];
	unsigned int  aad_len;
	unsigned char aad[TLSKEY_MAX_AAD];
	word_t        payload_off; /* absolute offset of the payload in the buffer */
	word_t        payload_len;
};

/* Validate (off_in, len) and copy the header out. Returns 0 or a TLSKEY_E* error. */
static long
hdr_read(struct tlskey_hdr *h, word_t off_in, word_t len)
{
	if (!buf) return TLSKEY_ENOBUF;
	if (!in_buf(off_in, len) || len < TLSKEY_HDR_LEN) return TLSKEY_ERANGE;
	memcpy(h->nonce, buf + off_in, TLSKEY_NONCE_LEN);
	h->aad_len = buf[off_in + 12] | (buf[off_in + 13] << 8);
	if (h->aad_len > TLSKEY_MAX_AAD || TLSKEY_HDR_LEN + h->aad_len > len) return TLSKEY_ERANGE;
	memcpy(h->aad, buf + off_in + TLSKEY_HDR_LEN, h->aad_len);
	h->payload_off = off_in + TLSKEY_HDR_LEN + h->aad_len;
	h->payload_len = len - TLSKEY_HDR_LEN - h->aad_len;

	return 0;
}

static struct tlskey_key *
key_get(word_t op_key)
{
	word_t id = op_key >> 8;

	if (id == 0 || id > TLSKEY_MAX_KEYS || !keys[id - 1].used) return NULL;

	return &keys[id - 1];
}

static long
op_setkey(word_t off, word_t len)
{
	unsigned char key[32];
	int i;

	if (!buf) return TLSKEY_ENOBUF;
	if (len != 16 && len != 32) return TLSKEY_EINVAL;
	if (!in_buf(off, len)) return TLSKEY_ERANGE;
	memcpy(key, buf + off, len); /* invariant 1: copied out once */
	for (i = 0; i < TLSKEY_MAX_KEYS; i++) {
		if (keys[i].used) continue;
		key_init(&keys[i], key, len);
		keys[i].used = 1;
		memset(key, 0, sizeof(key));
		return i + 1;
	}
	memset(key, 0, sizeof(key));

	return TLSKEY_ENOKEY;
}

static long
op_seal(struct tlskey_key *k, word_t off_in, word_t len, word_t off_out)
{
	struct tlskey_hdr h;
	long ret;

	if ((ret = hdr_read(&h, off_in, len))) return ret;
	if (!in_buf(off_out, h.payload_len + TLSKEY_TAG_LEN)) return TLSKEY_ERANGE;
	if (off_out != h.payload_off) memmove(buf + off_out, buf + h.payload_off, h.payload_len);

	br_gcm_reset(&k->gcm, h.nonce, TLSKEY_NONCE_LEN);
	br_gcm_aad_inject(&k->gcm, h.aad, h.aad_len);
	br_gcm_flip(&k->gcm);
	br_gcm_run(&k->gcm, 1, buf + off_out, h.payload_len);
	br_gcm_get_tag(&k->gcm, buf + off_out + h.payload_len);

	return 0;
}

static long
op_open(struct tlskey_key *k, word_t off_in, word_t len, word_t off_out)
{
	struct tlskey_hdr h;
	unsigned char tag[TLSKEY_TAG_LEN];
	word_t clen;
	long ret;

	if ((ret = hdr_read(&h, off_in, len))) return ret;
	if (h.payload_len < TLSKEY_TAG_LEN) return TLSKEY_ERANGE;
	clen = h.payload_len - TLSKEY_TAG_LEN;
	if (!in_buf(off_out, clen)) return TLSKEY_ERANGE;
	memcpy(tag, buf + h.payload_off + clen, TLSKEY_TAG_LEN); /* invariant 1 */
	if (off_out != h.payload_off) memmove(buf + off_out, buf + h.payload_off, clen);

	br_gcm_reset(&k->gcm, h.nonce, TLSKEY_NONCE_LEN);
	br_gcm_aad_inject(&k->gcm, h.aad, h.aad_len);
	br_gcm_flip(&k->gcm);
	br_gcm_run(&k->gcm, 0, buf + off_out, clen);
	if (!br_gcm_check_tag(&k->gcm, tag)) {
		memset(buf + off_out, 0, clen);
		return TLSKEY_EBADMSG;
	}

	return 0;
}

/*
 * AES-128-GCM, NIST GCM spec test case 2: K = 0^128, IV = 0^96, P = 0^128, no AAD.
 * Then test case 4 (the 60-byte message with 20 bytes of AAD) for the aad/flip path.
 */
static const unsigned char tc2_c[16] = {0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78};
static const unsigned char tc2_t[16] = {0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf};
static const unsigned char tc4_k[16] = {0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08};
static const unsigned char tc4_iv[12] = {0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88};
static const unsigned char tc4_p[60] = {
	0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
	0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
	0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
	0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39};
static const unsigned char tc4_a[20] = {
	0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xab,0xad,0xda,0xd2};
static const unsigned char tc4_c[60] = {
	0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,0x84,0xd0,0xd4,0x9c,
	0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,
	0x21,0xd5,0x14,0xb2,0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
	0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91};
static const unsigned char tc4_t[16] = {0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47};

static long
op_selftest(void)
{
	struct tlskey_key k;
	unsigned char zero[16] = {0}, m[60], tag[16];
	long ret = 0;

	key_init(&k, zero, 16);
	br_gcm_reset(&k.gcm, zero, 12);
	br_gcm_flip(&k.gcm);
	memset(m, 0, 16);
	br_gcm_run(&k.gcm, 1, m, 16);
	br_gcm_get_tag(&k.gcm, tag);
	if (memcmp(m, tc2_c, 16) || memcmp(tag, tc2_t, 16)) ret = TLSKEY_ESELF;

	key_init(&k, tc4_k, 16);
	br_gcm_reset(&k.gcm, tc4_iv, 12);
	br_gcm_aad_inject(&k.gcm, tc4_a, 20);
	br_gcm_flip(&k.gcm);
	memcpy(m, tc4_p, 60);
	br_gcm_run(&k.gcm, 1, m, 60);
	br_gcm_get_tag(&k.gcm, tag);
	if (memcmp(m, tc4_c, 60) || memcmp(tag, tc4_t, 16)) ret = TLSKEY_ESELF;

	/* and the OPEN direction on the same vector */
	br_gcm_reset(&k.gcm, tc4_iv, 12);
	br_gcm_aad_inject(&k.gcm, tc4_a, 20);
	br_gcm_flip(&k.gcm);
	br_gcm_run(&k.gcm, 0, m, 60);
	if (!br_gcm_check_tag(&k.gcm, tc4_t) || memcmp(m, tc4_p, 60)) ret = TLSKEY_ESELF;

	memset(&k, 0, sizeof(k));

	return ret;
}

word_t
tlskey_op(word_t op_key, word_t a, word_t b, word_t c)
{
	unsigned int mxcsr;
	struct tlskey_key *k;
	long ret;

	/* Invariant 2: the caller's MXCSR, restored by tlskey_leave. */
	asm volatile("stmxcsr %0" : "=m"(mxcsr));
	nerrands++;

	switch (op_key & 0xff) {
	case TLSKEY_OP_INFO_GPA: ret = buf ? (long)buf_gpa : TLSKEY_ENOBUF; break;
	case TLSKEY_OP_INFO_LEN: ret = buf ? (long)buf_len : TLSKEY_ENOBUF; break;
	case TLSKEY_OP_SELFTEST: ret = op_selftest(); break;
	case TLSKEY_OP_SETKEY:   ret = op_setkey(a, b); break;
	case TLSKEY_OP_SEAL:
		k = key_get(op_key);
		ret = k ? op_seal(k, a, b, c) : TLSKEY_EINVAL;
		break;
	case TLSKEY_OP_OPEN:
		k = key_get(op_key);
		ret = k ? op_open(k, a, b, c) : TLSKEY_EINVAL;
		break;
	case TLSKEY_OP_COUNTERS: ret = (long)nerrands; break;
	case TLSKEY_OP_DELKEY:
		k = key_get(op_key);
		if (k) memset(k, 0, sizeof(*k));
		ret = k ? 0 : TLSKEY_EINVAL;
		break;
	case TLSKEY_OP_ARGS:
		ret = (long)((a & 0xfffff) | ((b & 0xfffff) << 20) | ((c & 0xfffff) << 40));
		break;
	default: ret = TLSKEY_EINVAL; break;
	}

	tlskey_leave(mxcsr); /* invariant 2 */

	return (word_t)ret;
}

word_t
tlskey_attach(word_t shm_id, word_t gpa, word_t len, word_t unused)
{
	vaddr_t addr = 0;
	unsigned long npages;

	if (buf) return (word_t)TLSKEY_EBUSY;
	npages = memmgr_shared_page_map_aligned((cbuf_t)shm_id, PAGE_SIZE_4K, &addr);
	if (npages == 0 || addr == 0 || npages * PAGE_SIZE_4K < len) {
		printc("tlskey: attach of shm %lu failed (%lu pages at %lx for %lu bytes)\n", shm_id, npages, addr, len);
		return (word_t)TLSKEY_EINVAL;
	}
	buf_len = len;
	buf_gpa = gpa;
	buf     = (unsigned char *)addr;
	printc("tlskey: buffer attached: %lu KB, vaddr %p, guest-physical %lx\n", len / 1024, buf, gpa);

	return 0;
}

void
cos_init(void)
{
	const br_block_ctr_class *vt = br_aes_x86ni_ctr_get_vtable();

	printc("tlskey: component %ld, AES-NI %s, PCLMUL GHASH; %d key slots\n", cos_compid(),
	       vt ? "available" : "MISSING", TLSKEY_MAX_KEYS);
	assert(vt);
	printc("tlskey: GCM self-test %s\n", op_selftest() == 0 ? "passed" : "FAILED");
}
