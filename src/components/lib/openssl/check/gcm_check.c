/*
 * G1, host side: NIST GCM test cases 2 and 4 through EVP, with libcrypto.a built for the
 * component environment (configure-cos.sh) and linked with the tree's musl, run natively.
 * The per-record sequence is the service's (and libssl's TLS 1.3 record layer's): key once with
 * EVP_CipherInit_ex(ctx, EVP_aes_128_gcm(), NULL, key, NULL, enc); per record CipherInit(nonce),
 * one AAD update, one payload update in place, CipherFinal, GET_TAG / SET_TAG.
 * Also prints OPENSSL_ia32cap_P and a rough cycles/byte at 16 KB, so a run that silently fell
 * back to the C code is visible (not a measurement).
 */
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

extern unsigned int OPENSSL_ia32cap_P[4];

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

/* One record, in place: returns 1 on success (for OPEN: tag verified). */
static int
record(EVP_CIPHER_CTX *ctx, int enc, const unsigned char *iv, const unsigned char *aad, int aadlen,
       unsigned char *m, int len, unsigned char *tag)
{
	int outl;

	if (!EVP_CipherInit_ex(ctx, NULL, NULL, NULL, iv, enc)) return 0;
	if (!enc && !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, tag)) return 0;
	if (aadlen && !EVP_CipherUpdate(ctx, NULL, &outl, aad, aadlen)) return 0;
	if (len && (!EVP_CipherUpdate(ctx, m, &outl, m, len) || outl != len)) return 0;
	if (!EVP_CipherFinal_ex(ctx, m + len, &outl) || outl != 0) return 0;
	if (enc && !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag)) return 0;

	return 1;
}

static inline unsigned long long rdtsc(void) { unsigned a, d; asm volatile("lfence; rdtsc" : "=a"(a), "=d"(d)); return ((unsigned long long)d << 32) | a; }

int
main(void)
{
	static unsigned char big[16384];
	unsigned char zero[16] = {0}, m[60], tag[16];
	unsigned long long t0, best = ~0ULL;
	EVP_CIPHER_CTX *e = EVP_CIPHER_CTX_new(), *d = EVP_CIPHER_CTX_new();
	int ok = 1, ok2, ok4s, ok4o, bad;

	/* case 2: K = 0, IV = 0, P = 0^128, no AAD */
	EVP_CipherInit_ex(e, EVP_aes_128_gcm(), NULL, zero, NULL, 1);
	memset(m, 0, 16);
	ok2 = record(e, 1, zero, NULL, 0, m, 16, tag) && !memcmp(m, tc2_c, 16) && !memcmp(tag, tc2_t, 16);
	/* case 4: 60-byte message, 20 bytes of AAD; SEAL, then OPEN on a separate decrypt ctx */
	EVP_CipherInit_ex(e, EVP_aes_128_gcm(), NULL, tc4_k, NULL, 1);
	EVP_CipherInit_ex(d, EVP_aes_128_gcm(), NULL, tc4_k, NULL, 0);
	memcpy(m, tc4_p, 60);
	ok4s = record(e, 1, tc4_iv, tc4_a, 20, m, 60, tag) && !memcmp(m, tc4_c, 60) && !memcmp(tag, tc4_t, 16);
	memcpy(tag, tc4_t, 16);
	ok4o = record(d, 0, tc4_iv, tc4_a, 20, m, 60, tag) && !memcmp(m, tc4_p, 60);
	/* a corrupted tag must fail CipherFinal */
	memcpy(m, tc4_c, 60); memcpy(tag, tc4_t, 16); tag[0] ^= 1;
	bad = !record(d, 0, tc4_iv, tc4_a, 20, m, 60, tag);
	ok = ok2 && ok4s && ok4o && bad;

	printf("gcm_check: %s\n", OpenSSL_version(OPENSSL_VERSION));
	printf("gcm_check: OPENSSL_ia32cap_P = %08x %08x %08x %08x: AES-NI %d PCLMUL %d AVX %d MOVBE %d\n",
	       OPENSSL_ia32cap_P[0], OPENSSL_ia32cap_P[1], OPENSSL_ia32cap_P[2], OPENSSL_ia32cap_P[3],
	       !!(OPENSSL_ia32cap_P[1] & (1u << 25)), !!(OPENSSL_ia32cap_P[1] & (1u << 1)),
	       !!(OPENSSL_ia32cap_P[1] & (1u << 28)), !!(OPENSSL_ia32cap_P[1] & (1u << 22)));
	printf("gcm_check: NIST case 2 SEAL %s; case 4 SEAL %s, OPEN %s; corrupted tag %s\n",
	       ok2 ? "ok" : "WRONG", ok4s ? "ok" : "WRONG", ok4o ? "ok" : "WRONG", bad ? "rejected" : "ACCEPTED");

	for (int i = 0; i < 2000; i++) {
		t0 = rdtsc();
		record(e, 1, tc4_iv, tc4_a, 13, big, sizeof(big), tag);
		t0 = rdtsc() - t0;
		if (t0 < best) best = t0;
	}
	printf("gcm_check: 16 KB SEAL min %llu TSC cycles = %.2f cycles/B (sanity only)\n", best, (double)best / sizeof(big));
	printf("G1 RESULT: %s\n", ok ? "PASS" : "FAIL");
	EVP_CIPHER_CTX_free(e);
	EVP_CIPHER_CTX_free(d);

	return !ok;
}
