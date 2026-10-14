/*
 * tlskey errand ABI -- shared, byte for byte, between the Composite service
 * (src/components/interface/tlskey/tlskey_abi.h) and the guest
 * (cos-vmimg/programs/tlskey_abi.h). Plain C, no Composite or Linux includes.
 *
 * One errand: tlskey_op(op_key, a, b, c) -> one word (only rax comes back from an errand;
 * every result larger than a word goes through the shared buffer).
 *   op     = op_key & 0xff
 *   key id = op_key >> 8        (SETKEY returns it; 0 is never a valid key id)
 * Guest register ABI (see CLAUDE.md): rax = (64 + slot + 1) << 16, slot 1 for tlskey_op;
 * rbx = op_key, rsi = a, rdi = b, rdx = c.
 *
 * Buffer layout for SEAL/OPEN, at off_in:
 *   [12 B nonce][2 B aad_len, little endian][aad_len B aad][payload]
 * len (argument b) is the total length of that record. For SEAL the payload is plaintext and
 * the service writes ciphertext + 16 B tag at off_out. For OPEN the payload is ciphertext + tag
 * and the service writes plaintext at off_out. off_out may equal the payload's own offset
 * (off_in + 14 + aad_len): the transform is then in place and nothing is copied.
 *
 * Returns 0 (or a non-negative value for the INFO/COUNTERS/SETKEY/ARGS/SIGN/PUBKEY ops) or a negative
 * TLSKEY_E* error. Never a pointer.
 */
#ifndef TLSKEY_ABI_H
#define TLSKEY_ABI_H

#define TLSKEY_SLOT 1 /* errand slot; slot 0 stays pong_args */

#define TLSKEY_OP_INFO_GPA 1  /* -> guest-physical address of the shared buffer */
#define TLSKEY_OP_INFO_LEN 2  /* -> its length in bytes */
#define TLSKEY_OP_SELFTEST 3  /* AES-128-GCM on a fixed vector inside the service; -> 0 */
#define TLSKEY_OP_SETKEY   4  /* (off, len): key bytes in the buffer, len 16 or 32; -> key id */
#define TLSKEY_OP_SEAL     5  /* (off_in, len, off_out); key id in op_key >> 8 */
#define TLSKEY_OP_OPEN     6  /* (off_in, len, off_out); key id in op_key >> 8 */
#define TLSKEY_OP_COUNTERS 7  /* -> errands served since boot (this op included) */
#define TLSKEY_OP_DELKEY   8  /* key id in op_key >> 8; -> 0 */
#define TLSKEY_OP_ARGS     9  /* diagnostic: -> (a & 0xfffff) | (b & 0xfffff) << 20 | (c & 0xfffff) << 40 */
/* Phase 3 (w7): the server's ECDSA P-256 signing key, which exists only inside the service.
 * Added op numbers only; the record layout, ops 1-9 and the error codes are unchanged. */
#define TLSKEY_OP_SIGN    10 /* (off, len = 32, off_out): ECDSA over the 32-byte digest at off; the
                                 DER signature (at most TLSKEY_SIG_MAX B) at off_out; -> its length */
#define TLSKEY_OP_PUBKEY  11 /* (off_out): the signing key's public point, uncompressed, at off_out;
                                 -> TLSKEY_PUB_LEN */

#define TLSKEY_OPKEY(op, key_id) (((unsigned long)(key_id) << 8) | ((op) & 0xff))

#define TLSKEY_HDR_LEN   14 /* nonce + aad_len */
#define TLSKEY_NONCE_LEN 12
#define TLSKEY_TAG_LEN   16
#define TLSKEY_MAX_AAD   255
#define TLSKEY_MAX_KEYS  64
#define TLSKEY_DIGEST_LEN 32 /* SIGN: SHA-256 */
#define TLSKEY_SIG_MAX    72 /* SIGN: DER ECDSA-Sig-Value for P-256 */
#define TLSKEY_PUB_LEN    65 /* PUBKEY: 0x04 || x || y */

#define TLSKEY_EINVAL  (-1L) /* bad op, bad key id, length not 16/32 (SIGN: not 32, or no signing key) */
#define TLSKEY_ERANGE  (-2L) /* an (off, len) falls outside the buffer, or aad_len > TLSKEY_MAX_AAD */
#define TLSKEY_EBADMSG (-3L) /* OPEN: tag mismatch (the output region is zeroed) */
#define TLSKEY_ENOKEY  (-4L) /* SETKEY: no free key slot */
#define TLSKEY_ENOBUF  (-5L) /* no buffer attached yet */
#define TLSKEY_EBUSY   (-6L) /* attach: a buffer is already attached */
#define TLSKEY_ESELF   (-7L) /* SELFTEST: ciphertext or tag mismatch */

/* The VMM's census-dump port: `outb(tag, 0x505)` from the guest makes the VMM print its
 * per-reason count of slow (reflected) exits since boot and since the last dump. */
#define TLSKEY_CENSUS_PORT 0x505

#endif /* TLSKEY_ABI_H */
