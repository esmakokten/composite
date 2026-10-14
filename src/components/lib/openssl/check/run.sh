#!/bin/sh
# G1: build gcm_check with the component compile flags, link it statically against the
# component-built libcrypto.a and the tree's musl (crt1.o, libc.a, libgcc), run it natively.
# Usage: check/run.sh        (from lib/openssl/, after `make`)
set -eu
cd "$(dirname "$0")"
OSSL=../openssl-3.0.17
MUSL=../../libc/musl-1.2.0
CFLAGS="-m64 -mcmodel=large -O2 -nostdinc -isystem $MUSL/include -isystem $(gcc -print-file-name=include) \
-I$OSSL/include -fno-pic -fno-pie -fno-stack-protector -fno-omit-frame-pointer -Wall"

gcc $CFLAGS -c -o gcm_check.o gcm_check.c
gcc $CFLAGS -c -o cos_syscall_host.o cos_syscall_host.c
gcc -static -no-pie -nostdlib -o gcm_check $MUSL/lib/crt1.o $MUSL/lib/crti.o gcm_check.o cos_syscall_host.o \
	$OSSL/libcrypto.a $MUSL/lib/libc.a -lgcc $MUSL/lib/crtn.o
./gcm_check
