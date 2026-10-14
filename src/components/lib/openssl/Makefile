# OpenSSL 3.0.17's libcrypto for components (w7_openssl_service): the same EVP_aes_128_gcm code,
# with the same AES-NI/PCLMUL/AVX assembly, that the guest's nginx runs in its stock arm.
#
# Not vendored (60 MB unpacked): the tarball is taken from the workspace's third_party/ or
# fetched, checked against its sha256, extracted into openssl-$(OSSL_VER)/ and configured by
# configure-cos.sh (the one place the configure line lives), then only libcrypto.a is built.
# libopenssl.a is a thin archive over cos_openssl.o (the component's arena and init, see
# cos_openssl.h) and libcrypto.a's members, as lib/dpdk does. check/run.sh runs the NIST GCM
# vectors natively against libcrypto.a (G1).
LIBRARY_OUTPUT = openssl
OBJECT_OUTPUT =
INCLUDE_PATHS = . openssl-3.0.17/include
INTERFACE_DEPENDENCIES =
LIBRARY_DEPENDENCIES =

OSSL_VER     = 3.0.17
OSSL_SRC     = openssl-$(OSSL_VER)
OSSL_TARBALL = openssl-$(OSSL_VER).tar.gz
OSSL_URL     = https://github.com/openssl/openssl/releases/download/openssl-$(OSSL_VER)/$(OSSL_TARBALL)
OSSL_SHA256  = dfdd77e4ea1b57ff3a6dbde6b0bdc3f31db5ac99e7fdd4eaf9e1fbb6ec2db8ce
# the workspace keeps the tarball beside the checkout: ~/w7_openssl_service/third_party/
OSSL_LOCAL  ?= $(abspath ../../../../../third_party/$(OSSL_TARBALL))

include Makefile.lib

OBJS += $(OSSL_SRC)/libcrypto.a
# Makefile.lib's rule expanded its prerequisites before the line above
$(SHLIB_FILE): $(OSSL_SRC)/libcrypto.a
# cos_openssl.c needs the generated headers (opensslv.h, configuration.h)
cos_openssl.o: $(OSSL_SRC)/libcrypto.a
CINC += -I. -I$(OSSL_SRC)/include

$(OSSL_TARBALL):
	@if [ -f $(OSSL_LOCAL) ]; then cp $(OSSL_LOCAL) $@; else curl -fsSL -o $@ $(OSSL_URL); fi
	@echo "$(OSSL_SHA256)  $@" | sha256sum -c - >/dev/null || { rm -f $@; echo "openssl: bad tarball"; exit 1; }

$(OSSL_SRC)/configdata.pm: configure-cos.sh | $(OSSL_TARBALL)
	$(info |     [CONF] OpenSSL $(OSSL_VER) for the component environment (configure.log))
	@./configure-cos.sh $(OSSL_TARBALL) $(MUSLDIR)/include > configure.log 2>&1 || { tail configure.log; exit 1; }

$(OSSL_SRC)/libcrypto.a: $(OSSL_SRC)/configdata.pm
	$(info |     [MAKE] OpenSSL $(OSSL_VER) libcrypto.a (build.log))
	@$(MAKE) -s -C $(OSSL_SRC) build_generated > build.log 2>&1 || { tail build.log; exit 1; }
	@$(MAKE) -s -C $(OSSL_SRC) -j$(shell nproc) libcrypto.a >> build.log 2>&1 || { grep -m5 error: build.log; exit 1; }

distclean: openssl-distclean
.PHONY: openssl-distclean
openssl-distclean:
	@rm -rf $(OSSL_SRC) $(OSSL_TARBALL) configure.log build.log check/*.o check/gcm_check
