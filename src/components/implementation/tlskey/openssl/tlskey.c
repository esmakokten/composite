/*
 * tlskey: the TLS key service. A guest process (nginx) reaches tlskey_op through one vmcall
 * errand per TLS record; the key never leaves this component. The cipher is OpenSSL 3.0.17's
 * EVP AES-GCM (lib/openssl), the implementation and per-record call sequence that libssl runs
 * in nginx's stock arm. The invariants are written beside the code that upholds them:
 *
 *   1. Nothing read from the guest is trusted. Every (off, len) is checked against this
 *      component's own mapping of the buffer, and the part of a request the service
 *      interprets (nonce, aad_len, aad, key bytes, tag) is copied out once before it is used.
 *      The payload is only transformed in place: a guest that rewrites it concurrently gets a
 *      result it could have asked for directly, nothing more.
 *   2. No key material leaves in registers: the vector registers are zeroed and MXCSR is
 *      restored before every return from tlskey_op (tlskey_leave). libcrypto's AES-GCM is AVX
 *      (VEX) code on CPUs with AVX, so the scrub is vzeroall there (YMM0-15), the 16 pxor
 *      otherwise. Expanded keys live in this component's memory (the cos_openssl arena), which
 *      the guest cannot map.
 *   3. Only rax comes back from an errand. Every op returns one word; data goes through the
 *      buffer.
 *   4. The ABI, the buffer layout and the error codes are w5's, byte for byte (tlskey_abi.h is
 *      unchanged): a guest built for the BearSSL service runs against this one.
 *   5. The service and the stock arm execute the same cipher code: EVP AES-GCM from the same
 *      OpenSSL version, dispatched by OPENSSL_ia32cap_P to AES-NI + PCLMUL (and the AVX stitched
 *      aesni_gcm_* where AVX and MOVBE are present, as in the guest). cos_init refuses to run
 *      without AES-NI and PCLMUL and prints the capabilities it found.
 *   6. No file, socket, thread or dynamic loading: libcrypto is configured without them
 *      (lib/openssl/configure-cos.sh); its heap is a static arena (lib/openssl/cos_openssl.c).
 */
#include <cos_component.h>
#include <llprint.h>
#include <string.h>
#include <memmgr.h>
#include <tlskey.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <cos_openssl.h>

/*
 * One EVP_CIPHER_CTX per key slot, keyed once at SETKEY. GCM uses the same key schedule in both
 * directions, so the one ctx serves SEAL and OPEN: each record's CipherInit (the nonce, which
 * libssl issues per record anyway) also sets the direction. One ctx rather than one per
 * direction: half the memory, and the per-record calls are exactly libssl's.
 */
struct tlskey_key {
	int             used;
	EVP_CIPHER_CTX *ctx;
};

static struct tlskey_key keys[TLSKEY_MAX_KEYS];
/* Fetched once at cos_init, as libssl fetches its ciphers once per SSL_CTX (ssl_evp_cipher_fetch):
 * the implementations EVP_aes_128_gcm() and EVP_aes_256_gcm() name, without a fetch per SETKEY. */
static EVP_CIPHER *aes128gcm, *aes256gcm;
static int         have_avx; /* invariant 2: which scrub */

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

/*
 * Invariant 2: scrub the vector registers and restore the caller's MXCSR. The last thing before
 * return. With AVX, libcrypto's GCM ran VEX code: vzeroall clears YMM0-15 whole.
 */
static inline void
tlskey_leave(unsigned int mxcsr)
{
	if (have_avx) {
		asm volatile("vzeroall\n\t"
			     "ldmxcsr %0"
			     : : "m"(mxcsr)
			     : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
			       "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
		return;
	}
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

/* Key a slot once: AES-128-GCM or AES-256-GCM by key length. Returns 0 or -1 (no memory). */
static int
key_init(struct tlskey_key *k, const unsigned char *key, size_t len)
{
	if (!k->ctx && !(k->ctx = EVP_CIPHER_CTX_new())) return -1;
	if (!EVP_CipherInit_ex(k->ctx, len == 16 ? aes128gcm : aes256gcm, NULL, key, NULL, 1)) {
		EVP_CIPHER_CTX_free(k->ctx);
		k->ctx = NULL;
		return -1;
	}

	return 0;
}

static void
key_free(struct tlskey_key *k)
{
	EVP_CIPHER_CTX_free(k->ctx); /* cleanses the key schedule before the arena reuses it */
	k->ctx  = NULL;
	k->used = 0;
}

/*
 * One record, in place, libssl's TLS 1.3 per-record sequence: CipherInit(nonce, direction),
 * [OPEN] SET_TAG, one AAD update, one payload update, CipherFinal, [SEAL] GET_TAG into `tag`.
 * Returns 1, or 0 if the library refused or (OPEN) the tag did not verify.
 */
static int
gcm_record(EVP_CIPHER_CTX *ctx, int enc, const unsigned char *nonce, const unsigned char *aad,
	   unsigned int aad_len, unsigned char *m, word_t len, unsigned char *tag)
{
	unsigned char fin[16]; /* GCM's CipherFinal writes no bytes */
	int outl;

	if (len > 0x7fffffff) return 0; /* EVP lengths are int */
	if (!EVP_CipherInit_ex(ctx, NULL, NULL, NULL, nonce, enc)) return 0;
	if (!enc && !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, TLSKEY_TAG_LEN, tag)) return 0;
	if (aad_len && !EVP_CipherUpdate(ctx, NULL, &outl, aad, (int)aad_len)) return 0;
	if (len && (!EVP_CipherUpdate(ctx, m, &outl, m, (int)len) || (word_t)outl != len)) return 0;
	if (!EVP_CipherFinal_ex(ctx, fin, &outl)) {
		ERR_clear_error(); /* a bad tag queues an error; nothing else reads the queue */
		return 0;
	}
	if (enc && !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TLSKEY_TAG_LEN, tag)) return 0;

	return 1;
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
		if (key_init(&keys[i], key, len)) break; /* the arena is full: no slot either */
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

	if (!gcm_record(k->ctx, 1, h.nonce, h.aad, h.aad_len, buf + off_out, h.payload_len,
			buf + off_out + h.payload_len))
		return TLSKEY_EINVAL;

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

	if (!gcm_record(k->ctx, 0, h.nonce, h.aad, h.aad_len, buf + off_out, clen, tag)) {
		memset(buf + off_out, 0, clen);
		return TLSKEY_EBADMSG;
	}

	return 0;
}

/*
 * AES-128-GCM, NIST GCM spec test case 2: K = 0^128, IV = 0^96, P = 0^128, no AAD.
 * Then test case 4 (the 60-byte message with 20 bytes of AAD) for the AAD path, both directions.
 * Through key_init and gcm_record: the code SEAL and OPEN run.
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
	struct tlskey_key k = { 0 };
	unsigned char zero[16] = {0}, m[60], tag[16];
	long ret = 0;

	if (key_init(&k, zero, 16)) return TLSKEY_ESELF;
	memset(m, 0, 16);
	if (!gcm_record(k.ctx, 1, zero, NULL, 0, m, 16, tag) || memcmp(m, tc2_c, 16) || memcmp(tag, tc2_t, 16))
		ret = TLSKEY_ESELF;

	if (key_init(&k, tc4_k, 16)) { key_free(&k); return TLSKEY_ESELF; }
	memcpy(m, tc4_p, 60);
	if (!gcm_record(k.ctx, 1, tc4_iv, tc4_a, 20, m, 60, tag) || memcmp(m, tc4_c, 60) || memcmp(tag, tc4_t, 16))
		ret = TLSKEY_ESELF;

	/* and the OPEN direction on the same vector, then a corrupted tag that must fail */
	memcpy(tag, tc4_t, 16);
	if (!gcm_record(k.ctx, 0, tc4_iv, tc4_a, 20, m, 60, tag) || memcmp(m, tc4_p, 60)) ret = TLSKEY_ESELF;
	memcpy(m, tc4_c, 60);
	tag[0] ^= 1;
	if (gcm_record(k.ctx, 0, tc4_iv, tc4_a, 20, m, 60, tag)) ret = TLSKEY_ESELF;

	key_free(&k);
	memset(m, 0, sizeof(m));

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
		if (k) key_free(k);
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
	struct cos_openssl_caps caps;
	unsigned long used, peak, size;
	int ok;

	ok = cos_openssl_init() == 0;
	if (ok) {
		aes128gcm = EVP_CIPHER_fetch(NULL, "AES-128-GCM", NULL);
		aes256gcm = EVP_CIPHER_fetch(NULL, "AES-256-GCM", NULL);
	}
	cos_openssl_caps(&caps);
	have_avx = caps.avx;
	/* invariant 5: the assembly path the stock arm takes, or nothing */
	printc("tlskey: component %ld, %s, AES-NI %s, PCLMUL %s, AVX %d, MOVBE %d (stitched AES-GCM %s); %d key slots\n",
	       cos_compid(), OpenSSL_version(OPENSSL_VERSION), caps.aesni ? "available" : "MISSING",
	       caps.pclmul ? "available" : "MISSING", caps.avx, caps.movbe, caps.avx && caps.movbe ? "on" : "off",
	       TLSKEY_MAX_KEYS);
	assert(ok && aes128gcm && aes256gcm && caps.aesni && caps.pclmul);
	printc("tlskey: GCM self-test %s\n", op_selftest() == 0 ? "passed" : "FAILED");
	cos_openssl_heap(&used, &peak, &size);
	printc("tlskey: libcrypto arena %lu KB in use, peak %lu KB, of %lu KB\n", used / 1024, peak / 1024, size / 1024);
}

/* A server component with no main must still say so; nothing to run after initialisation. */
void
parallel_main(coreid_t cid)
{
}
