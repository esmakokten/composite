# tlskey

The TLS key service's errand interface: `tlskey_op` (the guest's slot-1 errand) and
`tlskey_attach` (the VMM hands over the shared buffer at set-up). Op codes, buffer layout and
error codes are in `tlskey_abi.h`, which is copied verbatim into the guest image builder
(`cos-vmimg/programs/tlskey_abi.h`).
