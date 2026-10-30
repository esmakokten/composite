# enclave

The system-call bounce as an errand interface: `enclave_op` (the guest's slot-4 errand) and
`enclave_attach` (the VMM hands over the shared buffer at set-up). Op codes, the record layout and
the error codes are in `enclave_abi.h`; the programs the component runs, with the timing macro
both arms share, are in `enclave_progs.h`. Both headers are copied verbatim into the guest image
builder (`cos-vmimg/programs/`), where `encbench.c` links the same programs natively.
