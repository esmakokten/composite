# IOMMU (Intel VT-d) support for Composite and the VMM

*Design spec — 2026-09-01. Baseline: `main` @ `b452fdf4`. Branch: `iommu-vtd`.*

## Problem

Composite has no IOMMU support of any kind. Every `iommu|dmar|vtd` occurrence in the
tree is inside vendored DPDK or musl headers; the kernel, the platform layer, and the
VMM contain none.

The consequence is concrete, not theoretical. `nicmgr` is a DPDK poll-mode driver
running as an ordinary Composite component. It obtains DMA addresses through
`memmgr_virt_to_phys` (`capmgr.c:254`), reached via `cos_map_virt_to_phys`
(`cos_dpdk_adapter.c:169`), and writes them into NIC descriptors as `rte_iova_t`
(`cos_dpdk.c:631`). IOVA is host-physical and unmediated: a wrong or hostile
descriptor address makes the NIC write anywhere in memory, kernel included. A
userlevel driver component therefore has, by way of its device, the authority of the
kernel.

`memmgr_map_phys_to_virt` (`capmgr.c:397`) compounds this by letting a component map
arbitrary physical memory directly.

## Goal

Give devices a translated, confined view of memory, staged in two phases:

- **Phase 1 — DMA remapping.** Confine a driver component's device to what that
  component can itself address. Demonstrated on `nicmgr`. No VMM changes.
- **Phase 2 — Interrupt remapping and passthrough.** MSI/MSI-X support, an interrupt
  remapping table, and device assignment into a guest VM. Sketched here, designed
  separately.

Phase 1 is a complete deliverable on its own. Phase 2 depends on it.

## Design decisions

### D1. A domain mirrors a component's address space

An **IOMMU domain** is a page table whose contents mirror one component's address
space. A driver component's device sees the component's *virtual* addresses; a VM
component's device sees *guest-physical* addresses, which is what that component's EPT
already contains.

The invariant is one sentence: **a device can reach exactly what its component can
reach, and no more.**

Alternatives rejected:

- *Identity (IOVA == host-physical, domain restricted to the component's frames).*
  Cheapest — no DPDK change at all — but it leaves `memmgr_virt_to_phys` in the
  interface as a live hole, and it does not generalise: a guest programs descriptors
  with guest-physical addresses, so Phase 2 would need a second mechanism.
- *Explicit registration (`dma_map`/`dma_unmap`).* Narrowest exposure window and the
  best audit story, but it rewrites DPDK's memory path, and under load it converges on
  the mirrored model anyway because per-packet mapping is ruinous for latency. It also
  has no answer for VM passthrough, where the guest cannot call a registration API.

Supporting evidence for the mirrored choice: DPDK has a first-class `--iova-mode=va`
for precisely this model. We are choosing the DPDK-native path, not fighting it.

### D2. The kernel owns tables, registers, and invalidation; policy is userlevel

The kernel owns:

- domain page tables, as a new capability-typed page table (D3),
- the root and context tables that bind a device to a domain,
- all DMAR register access,
- the invalidation queue and the quiescence extension (D4),
- DMAR ACPI table parsing.

A `dmarmgr` component owns device-to-domain assignment policy and fault consumption.

The register ownership needs justifying, because the original intent was to leave
registers to userlevel. It does not survive contact with the hardware: a component
that can write `RTADDR_REG` or the translation-enable bit in `GCMD_REG` chooses which
root table the hardware consults, which is the entire authority the design is trying
to confine. Validating a register base handed up from userspace would require the
kernel to parse the DMAR table itself in any case. So the kernel parses DMAR and owns
the registers. Registers turn out to belong to "tables and invalidation," not to
policy.

An IOMMU page table is a capability to DMA anywhere. A component that could write its
own raw entries could hand a device write access to kernel memory, so userlevel
construction of the mapping structures was never a real option either.

### D3. `PGTBL_TYPE_IOMMU` as a third page-table type

The kernel already carries a page-table type discriminant — `PGTBL_TYPE_DEF` and
`PGTBL_TYPE_EPT` in `chal/chal_proto.h:9-12`, with a `type` field on `struct
cap_pgtbl` — and VM components already receive EPT-typed roots through
`cos_pgtbl_alloc(ci, mem_type)` (`cos_kernel_api.c:1293`). A third type is the
idiomatic extension, and it inherits the existing allocation, expansion, and
capability machinery.

VT-d second-level page tables are structurally close to EPT (both put read, write, and
execute permissions in the low bits) but are not bit-identical, so the type needs its
own flag-translation layer, parallel to `x86_EPT_VM_DEF` in `chal_pgtbl.h:50`.

**Sharing rather than mirroring** — pointing a context-table entry directly at an
existing page table so that synchronisation is free by construction — is plausible for
the VM case in Phase 2, because EPT and VT-d second-level formats are deliberately
similar. It is not plausible for a regular component page table, whose low bits mean
present/writable/user rather than read/write/execute. Phase 1 therefore mirrors.
Whether Phase 2 can share must be settled against the VT-d specification before it is
designed around; it is not assumed here.

### D4. Invalidation is lazy, paid at retype

Composite does not do synchronous TLB shootdown. Unmap records a timestamp
(`retype_entry.last_unmap`); retype calls `tlb_quiescence_check(last_unmap)`
(`retype_tbl.c:329`) and throws `-EQUIESCENCE` for userspace to retry if no flush has
covered that timestamp on every core. Cost sits on the retype slow path and unmap
stays free.

The IOTLB analogue is structurally identical with one asymmetry that decides the
design: the CPU TLB is flushed for free by the periodic timer
(`chal_pgtbl.c:161`, `last_periodic_flush`), and nothing flushes the IOTLB for free.

We therefore invalidate **lazily, at retype, on demand**. Unmap continues to record
only a timestamp. When retype finds `last_unmap` newer than the last IOTLB flush, it
issues an invalidation and a wait descriptor at that point, updates the flush
timestamp, and proceeds. Fast paths are untouched; the queued-invalidation round trip
is paid only when a page is actually being reclaimed, which is already a slow path
permitted to retry.

Alternatives rejected: a timer-driven periodic global invalidate is perfectly
symmetric with the CPU-TLB story but adds an unconditional DMAR round trip whether or
not anything was unmapped, and bounds worst-case retype latency by the flush period;
synchronous invalidation at unmap puts a multi-microsecond round trip on the one path
the current design deliberately keeps free. Note that neither is what Linux does —
Linux batches through flush queues.

**Phase 1 granularity, stated precisely.** `retype_tbl` is keyed on physical frames
and has no knowledge of domains, so mapping a frame back to the domains that reach it
is not cheap. Phase 1 therefore keeps one global timestamp pair rather than per-domain
state:

- `iommu_last_unmap` is written on unmap **only when the page table being modified has
  an attached domain** — the back-pointer from K7 makes this a cheap test. Unmapping a
  page no device can reach leaves it untouched, so components with no assigned device
  pay nothing.
- `iommu_last_flush` records the completion of the last global IOTLB invalidate.
- Retype, after its existing `tlb_quiescence_check`, tests `iommu_last_unmap >
  iommu_last_flush`. If so it submits a global IOTLB invalidate and a wait descriptor,
  sets `iommu_last_flush` on completion, and proceeds.

This is coarse — one device-reachable unmap anywhere forces a global invalidate at the
next retype — but it is correct, and retype is a slow path already permitted to retry.
Per-domain granularity is a later refinement, not a correctness requirement.

## Consequences to accept

**A device inherits its component's sharing.** Mirroring `nicmgr`'s address space makes
its `shm_bm` and `netshmem` regions device-reachable, including pages shared with
netmgr clients. This follows correctly from D1, but it means the containment boundary
is the component *plus its shared memory*, not the component alone. This should be
stated plainly in any claim made about the property.

**IOPL is 3 for every component.** `chal_cpu.h:123` and the `r11=0x3200` on every
`sysret` give every component port I/O, hence PCI configuration space, hence the
ability to reprogram any device's BARs. An IOMMU without mediated configuration access
buys containment against a buggy or compromised *device*, not against a malicious
driver *component*. Phase 1 does not close this, and claims must be scoped to match.

**RMRRs.** Firmware reserves memory regions that certain devices — USB controllers,
integrated graphics — require identity-mapped or they malfunction. Phase 1 refuses to
bind any device carrying an RMRR and logs it, rather than silently honouring reserved
regions.

## Phase 1 work breakdown

### Kernel

- **K1 — XSDT support in `miniacpi.c`.** The parser is RSDT-only with 32-bit table
  pointers (`miniacpi.c:102`, `:127`). Modern firmware may publish no RSDT at all.
  Use `rsdp->revision` to select XSDT and 64-bit entries. Hard prerequisite; verify
  first, it is cheap and it gates everything.
- **K2 — DMAR table parse.** DRHD units (register base, segment, device scope,
  include-all flag) and RMRR regions, into a bounded static array.
  `acpi_find_resource("DMAR")` works unmodified once K1 lands.
- **K3 — Register mapping and capability read.** Map each unit's register set with the
  existing `device_map_mem` (`vm.c:145`). Read the capability and extended capability
  registers for supported address widths, domain count, queued-invalidation support,
  and page-walk coherency. Exact offsets and bit positions must be taken from the VT-d
  specification during implementation, not from this document.
- **K4 — `PGTBL_TYPE_IOMMU`.** New type in `chal/chal_proto.h`, a VT-d second-level
  flag-translation layer beside the EPT one in `chal_pgtbl.h`, and wiring into the
  type switches in `chal_pgtbl.c` (approximately lines 248, 730, 772) and the page
  table activate path.
- **K5 — Root and context tables.** Kernel-allocated. A context entry carries a domain
  identifier, the second-level table pointer, and an address width.
- **K6 — Bind operation.** `CAPTBL_OP_IOMMU_BIND(bdf, pgtbl_cap)`: validate the page
  table is IOMMU-typed, allocate a domain identifier, write the context entry,
  invalidate the context cache.
- **K7 — Mirroring hook.** Paired domain-table update on `pgtbl_mapping_add` and
  `pgtbl_mapping_del` when the component has an attached domain. Requires a
  back-pointer from a component page table to its domain. On bind, the domain is built
  from the component's current mappings; subsequent changes are mirrored.
- **K8 — Invalidation queue.** Queue allocation, IOTLB-invalidate and
  invalidation-wait descriptor submission, and the flush timestamp of D4.
- **K9 — Quiescence extension.** Retype path issues invalidation on demand per D4, at
  the global granularity described there.
- **K10 — Fault recording.** Polled for Phase 1. Fault-reporting by interrupt requires
  Phase 2's MSI work, because external interrupts are today capped at legacy lines
  32–63 (`hw.h:17-18`).

### Userlevel

- **U1 — `dmarmgr` component.** Interface for domain creation, device binding, and
  fault polling.
- **U2 — Assignment configuration.** Which BDF is bound to which component, expressed
  in `composition_scripts/` and `tools/gen_cdf.py` alongside the rest of the system
  composition.
- **U3 — `nicmgr` and the DPDK glue.** `cos_map_virt_to_phys` collapses to identity.
  Confirm the vendored DPDK's `--iova-mode=va` path works through the Composite EAL
  glue.
- **U4 — Close the physical-address interface.** Remove or restrict
  `memmgr_virt_to_phys` and `memmgr_map_phys_to_virt` once no caller needs them.

## Validation

**Safety, and the experiment that would falsify it.** Point a NIC descriptor at a
kernel physical address. The claim holds if a DMA remapping fault is recorded and
memory is unchanged; it fails if memory is corrupted. Run before and after the change,
so the pre-change corruption is demonstrated rather than assumed.

**Cost.** Packet-path latency with and without translation, reported as a
distribution rather than a mean, since the claim of interest is tail behaviour. And
retype latency when an invalidation is pending, which is where D4 deliberately places
the cost.

**Environments.** QEMU needs `-M q35,kernel-irqchip=split -device intel-iommu` and a
real emulated NIC; `tools/run.sh:55` currently uses the default machine type and
`-nic none`, so the run script is part of the deliverable. Bare metal is the A03
target, a Dell R740 with two Xeon Platinum 8160s, which requires VT-d enabled in BIOS
and, being two-socket, exercises multi-DRHD routing that QEMU will not.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| No RSDT on R740 firmware | Blocks everything | K1 first; verify on hardware before further work |
| IOMMU not page-walk coherent | Every table write needs a cache flush | Read the coherency capability in K3; branch the write path |
| Multi-DRHD routing | Invalidation sent to the wrong unit is silently wrong | Bind each device to the unit its DMAR scope names; test on the two-socket target, not only QEMU |
| Vendored DPDK lacks working `--iova-mode=va` | U3 stalls | Check the vendored version early; identity mapping is the fallback |
| Mirroring hook races page-table updates | Correctness | Domain update inside the same critical section as the component mapping change |

## Phase 2 sketch

Not designed here. It requires MSI and MSI-X capability parsing, an interrupt model
beyond the legacy 32–63 range with a send endpoint per vector, an interrupt remapping
table, and device assignment into a VM. The piece that matters for the low-overhead
virtualisation argument is VT-d posted interrupts, which deliver a device interrupt
into a running guest without an exit — available on the Skylake-SP target. Whether a
VM's domain can share its EPT outright, rather than mirror it, is the first question
Phase 2 should settle.
