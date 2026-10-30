# The system-call bounce as a microbenchmark (w9): a program inside this component makes its
# system calls through the guest, one errand per call (slot 4).
INTERFACE_EXPORTS = enclave
INTERFACE_DEPENDENCIES = memmgr
LIBRARY_DEPENDENCIES = component
# -O2: the bounce's cost is the measurement; the tree builds components at -O0 by default.
# -mgeneral-regs-only: no vector registers in this component (CLAUDE.md invariant 5), so the
# vector-state defect (vault Known Bugs §2) cannot reach it through its own registers.
COMP_FLAGS = -O2 -mgeneral-regs-only

include Makefile.subsubdir
