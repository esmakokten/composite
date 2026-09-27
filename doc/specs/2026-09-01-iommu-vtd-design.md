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

Two things are easy to conflate here, and the rest of this document depends on keeping
them apart:

- **Safety** — a device must never reach memory the kernel has handed to someone else.
  This is a kernel property and it is already enforced, for free, by D2.
- **Coherence** — the domain's table and the component's own table describe the same
  memory. This is a *policy* property. Getting it wrong means the component's device
  cannot reach a buffer the component expects it to, or retains reach to a page the
  component has stopped using. Neither breaks kernel integrity.

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

### D2. The kernel does retype, construction and mapping; user level keeps the tables coherent

The kernel owns:

- domain page tables, as a capability-typed page table (D3) — retyped, constructed and
  mapped through exactly the machinery that already exists for every other page table,
  with no IOMMU-specific path,
- the root and context tables that bind a device to a domain,
- all DMAR register access,
- the invalidation queue, and IOTLB quiescence on the retype path (D4),
- DMAR ACPI table parsing.

The memory and capability manager owns:

- which component gets a domain, and which device is bound to it,
- **keeping a component's IOMMU page table coherent with its own page table** — for a
  VM, keeping the IOMMU table coherent with the EPT,
- fault consumption.

**Why coherence is not kernel work.** Because safety does not need it. Mapping a frame
into any page table calls `retypetbl_ref()` on that frame
(`chal_pgtbl.c:410`), and the reference count is keyed on the *physical frame* — it
does not record which page table the mapping went into. A frame mapped into an
`IOMMU`-typed table therefore carries a reference like any other, and
`retypetbl_retype2untyped` already refuses to reclaim it while the count is non-zero.

So the kernel cannot let a device-reachable page be recycled, and it gets that property
without knowing that IOMMU tables exist. Adding a kernel hook to mirror one table into
another would buy nothing for safety and would put policy in the kernel. An earlier
draft of this document did exactly that, as K7; it was wrong, and K7 is now user-level
work.

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
the VM case, because EPT and VT-d second-level formats are deliberately similar. It is not plausible for a regular component page table, whose low bits mean
present/writable/user rather than read/write/execute. A driver component's table must
therefore be mirrored. Whether a *VM's* EPT can be shared outright is a Phase 1
question, not a Phase 2 one, because a favourable answer deletes U5 — see
**Open question: one table instead of two** below.

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

### D5. Binding a device takes a reference on the IOMMU page table

A context entry holds the *physical address* of a second-level page table root. That is
a reference the capability system cannot see: if the owning component dies and the table
is freed and retyped, the context entry still points there and the device DMAs into
reused memory. The frame reference counts of D2 do not close this, because the context
entry is a kernel structure holding a raw pointer, not a mapping.

The fix follows the same principle as the rest of the design. `chal_pgtbl_cons` already
increments `refcnt_flags` on a sub-table when a page table is made to point at it; a
context entry pointing at a page table is structurally the same relationship. **Binding
a device takes a reference on the IOMMU page-table capability, and unbinding releases
it.** The table cannot then be deactivated or retyped while a device is still attached.

### D6. Domain identifiers are tracked in the kernel

A domain identifier is a per-unit number written into context entries and into
invalidation descriptors. It is bounded by the unit's capability register — 65536 on
both measured targets — and is only ever allocated at bind time, so allocation is not
on any fast path.

**It is tracked in the kernel, in a fixed array indexed by identifier**, following the
LAPIC identifier precedent (`apicids[NUM_CPU]` in `lapic.c:41`): bounded, static,
populated as devices are bound, no dynamic allocation.

The alternative — allocating identifiers at user level and passing them in, the way
`protdom_ns_asid` allocates ASIDs in `llbooter.c:61` and hands them to `comp_activate`
inside `prot_domain_t` — was considered and rejected. IOTLB entries are tagged by domain
identifier, so two different second-level tables sharing an identifier lets one device
hit another's cached translation. A collision is an isolation failure, not a performance
bug. That is the same failure mode the PCID/ASID collision in this codebase already
produced, which makes the user-level precedent a cautionary tale as much as a template.

If the identifier space is later moved to user level for consistency, the kernel must
still reject a bind whose identifier is already live on that unit — which requires the
same table, and yields nothing.

### D7. A VM component holds two page-table capabilities

A component that has a device is created with a capability to the top level of its own
page table *and* to the top level of its IOMMU page table, established together at
`comp_activate` so the pairing exists from birth rather than being asserted later.

The space is tight and worth stating precisely rather than estimating. Measured on the
built object:

| | bytes |
|---|---|
| `sizeof(struct cap_comp)` | 60 |
| `CAP_COMP` size class | 64 |
| `struct cap_pgtbl *` | 8 |

**Four bytes of slack**, so a second kernel pointer does not fit — by exactly four
bytes. A 4-byte capability id or frame number does fit, and costs only a table lookup
on a path that runs at bind time and on mapping changes, never on a fast path. The
separate page for storing these, contemplated as a last resort, is therefore not
needed.

Before implementing even that, one question should be settled: **what does the kernel
do with the IOMMU root that the manager could not do with its own bookkeeping?** Under
D2 the kernel does not maintain coherence, so if the pairing exists only so that the
manager knows which two tables belong together, the manager can track it and the field
is unnecessary. The one argument for kernel-side storage is teardown, and D5 handles
teardown better.

## Open question: one table instead of two

D1 assumes two tables kept coherent. If a context entry could point directly at a VM's
existing EPT root, coherence would be free by construction, the manager would have
nothing to maintain, and D2's user-level obligation would disappear entirely.

The evidence so far is encouraging but incomplete. EPT places read, write and execute
at bits 0, 1 and 2; the phase-three self-test confirmed VT-d second-level entries do the
same. The divergence is EPT's bits 5:3 memory type and bit 6 ignore-PAT against VT-d's
extended-memory-type field and snoop bit.

This must be settled against the VT-d specification before the manager-side coherence
tracking is built, because settling it favourably deletes that work rather than
informing it.

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
  table is IOMMU-typed, refuse a device covered by an RMRR, allocate a domain
  identifier from the kernel table (D6), **take a reference on the page-table
  capability** (D5), write the context entry, flush it, and invalidate the context
  cache of the unit that owns the device. Unbind releases the reference.
- **K7 — moved to user level.** Was a kernel mirroring hook. Coherence between a
  component's page table and its IOMMU table is policy, not safety, and belongs in the
  memory manager — see D2 and U5. The identifier is retained rather than renumbered so
  that the plan documents referring to K5–K10 continue to resolve.
- **K11 — Component pairing.** `comp_activate` accepts and records the IOMMU
  page-table root alongside the existing one, per D7 — *if* the open question above
  leaves the kernel needing it at all.
- **K8 — Invalidation queue.** Queue allocation, IOTLB-invalidate and
  invalidation-wait descriptor submission, and the flush timestamp of D4.
- **K9 — Quiescence extension.** Retype path issues invalidation on demand per D4, at
  the global granularity described there.
- **K10 — Fault recording.** Polled for Phase 1. Fault-reporting by interrupt requires
  Phase 2's MSI work, because external interrupts are today capped at legacy lines
  32–63 (`hw.h:17-18`).

### Userlevel

- **U1 — Manager responsibilities, and where they live.** Two distinct jobs, and only
  one of them has a forced home. *Coherence* (U5) must sit wherever map and unmap
  happen, because the paired IOMMU-table change has to be made under the same lock —
  that is `capmgr`, and it is not a free choice. *Assignment policy and fault polling*
  could live in a separate `dmarmgr`, but splitting them means two components need
  authority over the same domain capability, so the default is to put them in `capmgr`
  too and split only if a reason appears.
- **U2 — Assignment configuration.** Which BDF is bound to which component, expressed
  in `composition_scripts/` and `tools/gen_cdf.py` alongside the rest of the system
  composition.
- **U3 — `nicmgr` and the DPDK glue.** `cos_map_virt_to_phys` collapses to identity.
  Confirm the vendored DPDK's `--iova-mode=va` path works through the Composite EAL
  glue.
- **U4 — Close the physical-address interface.** Remove or restrict
  `memmgr_virt_to_phys` and `memmgr_map_phys_to_virt` once no caller needs them.
- **U5 — Coherence tracking (was K7).** The memory manager maintains the invariant that
  a component's IOMMU page table describes the same memory as the component's own page
  table: on every map and unmap it performs for a component that has a domain, it makes
  the paired change to that component's IOMMU table. Because the kernel refuses to
  reclaim a frame that is still mapped anywhere (D2), a lapse here is a correctness bug
  in the manager's own service, not a breach of kernel memory safety. Unnecessary if
  the open question above resolves in favour of one table.

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
| Manager's coherence tracking races its own map/unmap | The component's device sees a stale view | Manager makes the paired IOMMU-table change under the same lock it already holds for the mapping; a lapse is confined to that component (D2) |
| Domain identifier collision across two page tables | Isolation failure — one device hits another's cached translation | Identifiers allocated in the kernel (D6), uniqueness by construction |
| Context entry outlives the page table it names | Device DMAs into reclaimed memory | Binding holds a reference on the page-table capability (D5) |

## Measured hardware facts

Established by the foundation work on branch `iommu-vtd`, replacing the assumptions
this spec was written under.

| | QEMU `q35` + `intel-iommu` | Lab R740 (2x Xeon Platinum 8160) |
|---|---|---|
| Remapping units | 1 | **4** |
| Register base | `0xfed90000` | `0x9d7fc000`, `0xaaffc000`, `0xb87fc000`, `0xc5ffc000` |
| Version | 1.0 | 1.0 |
| Domains | 65536 | 65536 |
| Address width / SAGAW | 39-bit, 3-level (`0x2`) | **48-bit, 4-level (`0x4`)** |
| Queued invalidation | yes | yes |
| Page-walk coherency (ECAP.C) | **no** | **yes** |

Four consequences.

**Multi-unit routing is mandatory, not a refinement.** The R740 presents four units, and
QEMU's single unit does not even set `INCLUDE_PCI_ALL` — it routes by explicit device
scope. A device must be resolved to its owning unit before any invalidation is sent to
it, since sending one to the wrong unit is silently wrong. `dmar_unit_for_bdf()` does
this.

**The two test environments exercise opposite coherency paths.** QEMU reports no
page-walk coherency, so every write to a remapping structure must be flushed from the
cache before the hardware reads it; the R740 reports coherency and needs no flush.
Neither environment alone covers both paths, so the flush path must be conditional on
`ECAP.C` and both settings must be exercised. This is fortunate rather than
inconvenient: the harder path is the one QEMU forces, so it gets tested by default.

**Page-table depth differs between environments.** Three levels under QEMU against four
on hardware. Domain page tables must derive their depth from SAGAW rather than assuming
the host's own paging depth.

**Queued invalidation is available everywhere measured**, so the lazy-invalidation
design in D4 has the mechanism it assumes on both.

## Phase 2 sketch

Not designed here. It requires MSI and MSI-X capability parsing, an interrupt model
beyond the legacy 32–63 range with a send endpoint per vector, an interrupt remapping
table, and device assignment into a VM. The piece that matters for the low-overhead
virtualisation argument is VT-d posted interrupts, which deliver a device interrupt
into a running guest without an exit — available on the Skylake-SP target. Whether a
VM's domain can share its EPT outright is settled in Phase 1, not here.
