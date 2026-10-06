#ifndef TLSKEY_H
#define TLSKEY_H

#include <cos_component.h>
#include <tlskey_abi.h>

/*
 * The errand entry (slot 1 in the VM's captbl) and the VMM's set-up call. Both are plain
 * 4-argument, 1-return invocations (cos_asm_stub) so that the server receives the guest's
 * rbx, rsi, rdi, rdx as the four parameters and returns rax.
 */
word_t tlskey_op(word_t op_key, word_t a, word_t b, word_t c);
/* VMM -> service, once: map shared-page object `shm_id` (the buffer the VMM aliased into the
 * VM at guest-physical `gpa`, `len` bytes). Returns 0 or a negative TLSKEY_E*. */
word_t tlskey_attach(word_t shm_id, word_t gpa, word_t len, word_t unused);

#endif /* TLSKEY_H */
