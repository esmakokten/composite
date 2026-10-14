# The TLS key service: holds AES-GCM session keys outside the VM and runs the cipher for the
# guest's TLS records, entered by a vmcall errand (slot 1). OpenSSL 3.0.17's EVP AES-GCM (lib/openssl).
INTERFACE_EXPORTS = tlskey
INTERFACE_DEPENDENCIES = memmgr
LIBRARY_DEPENDENCIES = component openssl
# -O2: the errand path's cost is the measurement; the tree builds components at -O0 by default.
COMP_FLAGS = -O2

include Makefile.subsubdir
