# BearSSL 0.6 (https://bearssl.org/bearssl-0.6.tar.gz,
# sha256 6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14), vendored
# unmodified under bearssl/ (inc/ and src/ only; tests, tools and the .t0 sources are dropped).
#
# Built with -maes -mpclmul -msse4.1 so that the AES-NI block ciphers (aes_x86ni_*) and the
# PCLMUL GHASH (ghash_pclmul) compile; the portable constant-time code is ~20x slower and would
# hide the errand crossing inside the cipher. Callers select the x86ni vtables explicitly
# (br_aes_x86ni_ctr_get_vtable(), br_ghash_pclmul); they are only valid on CPUs with AES-NI.
LIBRARY_OUTPUT = bearssl
OBJECT_OUTPUT =
INCLUDE_PATHS = bearssl/inc
INTERFACE_DEPENDENCIES =
LIBRARY_DEPENDENCIES =

CFILES += $(shell find bearssl/src -name '*.c' | sort)

include Makefile.lib

# -nostdinc hides gcc's own headers; the x86 intrinsics (<x86intrin.h>, <cpuid.h>) live there.
CINC += -Ibearssl/inc -Ibearssl/src -I$(shell $(CC) -print-file-name=include)
CFLAGS += -O2 -maes -mpclmul -msse4.1 -mssse3 -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough
