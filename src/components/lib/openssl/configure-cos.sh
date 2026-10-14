#!/bin/sh
# Extract OpenSSL 3.0.17 and configure it for the component environment: static libcrypto, no
# threads, files, sockets, dynamic loading or console, asm on (AES-NI, PCLMUL, the AVX stitched
# AES-GCM), the tree's musl headers instead of the host's, no PIC, the large code model the
# components are compiled with. (3.0.17 has no no-apps/no-docs; only libcrypto.a is built, so
# neither is compiled.) The one place the configure line lives (CLAUDE.md trap 3):
# a re-configure always starts from a fresh extraction, so stale configdata.pm cannot survive.
#
# Usage: configure-cos.sh <tarball> <musl include dir>      (run from lib/openssl/)
set -eu

VER=3.0.17
SHA256=dfdd77e4ea1b57ff3a6dbde6b0bdc3f31db5ac99e7fdd4eaf9e1fbb6ec2db8ce
TARBALL=$1
MUSLINC=$2
SRC=openssl-$VER

echo "$SHA256  $TARBALL" | sha256sum -c -
rm -rf "$SRC"
tar -xzf "$TARBALL"

# The component CFLAGS (Makefile.comp + Makefile.arch) that matter for code generation and
# headers. gcc's own include dir comes *after* musl's, so musl's stddef.h/stdarg.h win and only
# the intrinsics (wmmintrin.h, ...) come from gcc (CLAUDE.md trap 2).
GCCINC=$(gcc -print-file-name=include)
CFLAGS="-m64 -mcmodel=large -O3 -nostdinc -isystem $MUSLINC -isystem $GCCINC \
-fno-pic -fno-pie -fno-stack-protector -fno-omit-frame-pointer -fno-merge-constants"

cd "$SRC"
./Configure linux-x86_64 \
	no-shared no-dso no-module no-pic no-threads no-async no-engine no-dynamic-engine \
	no-afalgeng no-sock no-dgram no-stdio no-posix-io no-ui-console no-autoload-config \
	no-filenames no-comp no-tests no-legacy \
	CC=gcc CFLAGS="$CFLAGS" LDFLAGS= LDLIBS=
perl configdata.pm -o
