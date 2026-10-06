# bearssl

BearSSL 0.6, vendored unmodified (see `Makefile` for the source URL and sha256). Used by the
`tlskey` service for AES-128-GCM (`br_aes_x86ni_ctr_*` + `br_ghash_pclmul`) and, later, ECDSA
P-256. Needs only `memcpy`, `memmove`, `memcmp`, `memset` and `strlen` from libc.
