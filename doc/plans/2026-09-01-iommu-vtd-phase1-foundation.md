# IOMMU (VT-d) Phase 1 Foundation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Composite kernel discover the VT-d hardware, decode its capabilities, and allocate IOMMU-typed page tables — the foundation every later Phase 1 task builds on.

**Architecture:** Extend the existing ACPI parser to reach the DMAR table, parse DRHD and RMRR structures into bounded static state, map each unit's registers with the existing `device_map_mem` path, and add `PGTBL_TYPE_IOMMU` as a third capability-typed page table beside `PGTBL_TYPE_DEF` and `PGTBL_TYPE_EPT`.

**Tech Stack:** C (kernel, `src/platform/x86_64/`), Composite capability/page-table machinery, QEMU `q35` with `-device intel-iommu` for the test environment, Dell R740 (2× Xeon Platinum 8160) for bare metal.

**Spec:** `doc/specs/2026-09-01-iommu-vtd-design.md`

## Scope of this plan, and what it deliberately excludes

The spec's Phase 1 has ten kernel items (K1–K10) and four userlevel items (U1–U4). **This plan covers K1–K4 plus the test environment.** It stops at the point where the kernel can discover the IOMMU, report what the hardware supports, and allocate an IOMMU-typed page table.

That boundary is deliberate, not arbitrary. The design of K5–K10 depends on facts this plan establishes and nobody currently knows:

- whether Composite boots at all on QEMU's `q35` machine type (it has only ever run on the default `pc`),
- whether the target's firmware publishes an RSDT or only an XSDT,
- whether the IOMMU units report page-walk coherency, which decides whether every table write needs a cache flush,
- how many DRHD units the two-socket R740 presents and what falls under each one's scope.

Writing detailed code for the mirroring hook and the invalidation queue before those answers exist would be speculation dressed as a plan. A second plan covers K5–K10 and U1–U4 once this one lands.

## Global Constraints

- **`src/platform/x86_64/` is mostly symlinks into `src/platform/i386/`.** There is no parallel 32-bit copy to leave alone: `miniacpi.c`, `kernel.c`, `chal_pgtbl.c`, `chal_pgtbl.h`, and `chal/chal_proto.h` are all one shared file, differentiated by `#if defined(__x86_64__)` / `#elif defined(__i386__)`. `Makefile` and `vm.c` are genuinely x86_64's own. Before editing any platform file, run `readlink src/platform/x86_64/<file>` and know which you are touching.
- **Every shared-file edit must leave the 32-bit build behaviourally unchanged.** `paddr_t` is `unsigned long`, so it is 32 bits on i386 and 64 on x86_64: anything 64-bit-address-shaped needs an `#if defined(__x86_64__)` guard rather than a silent truncation. New x86_64-only code (`dmar.c`) is added to the x86_64 `Makefile` only, and its call sites in shared files are guarded.
- **The VMX support this work eventually serves lives only under `src/platform/x86_64/vmx/`.**
- **Hardware constants come from the VT-d specification, not from this document.** Register offsets, capability-register bit positions, and descriptor layouts are named here by their spec names and left for the implementer to fill in from *Intel Virtualization Technology for Directed I/O, Architecture Specification*. Any constant written speculatively into this plan would look authoritative and be wrong. Where this plan shows a struct or a decode, treat the field names as the contract and the numeric values as to-be-sourced.
- **Bounded static allocation only.** The kernel has no dynamic allocator at this stage of boot. All DMAR state lives in fixed-size arrays with a compile-time maximum and a fail-fast assert on overflow, following the existing `DEV_MAPS_MAX` pattern in `src/platform/x86_64/vm.c:114`.
- **Every kernel printk added for verification is permanent, not scaffolding.** Boot-time reporting of what the IOMMU hardware is and what it supports is the only observability this subsystem will have until fault reporting lands in K10. Write it to be read.
- **Commit after every task.** No task depends on an uncommitted predecessor.

## The verification loop

There is no unit-test runner for kernel code in this repository. The actual cycle is:

```bash
cd /home/esma/workspace/composite_main
./cos init x86_64          # once per checkout
./cos build
./cos compose composition_scripts/<name>.toml <build-name>
./cos run <build-name> 2>&1 | tee /tmp/boot-<build-name>.log
```

**Note on composition scripts:** 14 of the `.toml` files on `main`, `unit_memmgr.toml` among them, reference `sched.root_fprr`, which does not exist in this tree — only `sched.pfprr_quantum_static` does. Those compositions fail at the component-build stage. `unit_heap.toml` is used as the baseline here because it is small, memory-focused, and references the scheduler that exists. Any new `.toml` must do the same.

A "test" for a kernel task is therefore a specific line the kernel must print at boot, asserted by grepping that log. This is weaker than a unit test and the plan does not pretend otherwise — but the order still holds: write the assertion, run it and watch it fail, implement, run it and watch it pass.

For component-level tasks the repository does have a convention worth following: a `tests/unit_*` component that prints `SUCCESS:` or `FAILURE:` lines (see `src/components/implementation/tests/unit_memmgr/`), wired into a `.toml` in `composition_scripts/`.

## File structure

| File | Responsibility | Status |
|---|---|---|
| `tools/run.sh` | QEMU invocation; gains a machine-type and IOMMU option | Modify |
| `src/platform/i386/miniacpi.c` (x86_64 symlinks to it) | RSDP/RSDT/XSDT walk, table lookup by signature | Modify |
| `src/platform/x86_64/dmar.h` | DMAR/DRHD/RMRR structure definitions and the parsed-state interface | Create |
| `src/platform/x86_64/dmar.c` | DMAR table parse, register mapping, capability decode | Create |
| `src/platform/i386/chal/chal_proto.h` (shared) | Page-table type constants | Modify |
| `src/platform/i386/chal_pgtbl.h` (shared) | VT-d second-level flag definitions, beside the EPT ones | Modify |
| `src/platform/i386/chal_pgtbl.c` (shared) | Page-table type dispatch | Modify |
| `src/platform/i386/kernel.c` (shared) | Boot sequence; guarded call to DMAR init after ACPI init | Modify |

`dmar.c` is a new file rather than an addition to `miniacpi.c` because the two have different jobs: `miniacpi.c` finds tables, `dmar.c` interprets one specific table and owns the hardware behind it. Keeping DMAR register access in one file also makes the "kernel owns all DMAR registers" decision from the spec visible in the source layout rather than only in a document.

---

### Task 1: QEMU test environment with an emulated IOMMU

Nothing downstream can be tested without an IOMMU present in the test machine. This task also answers the first open question in the plan's scope: whether Composite boots on `q35` at all.

**Files:**
- Modify: `tools/run.sh:55`

**Interfaces:**
- Consumes: nothing.
- Produces: a `./cos run <name> iommu` invocation that boots Composite on `q35` with `-device intel-iommu`, used by every later task.

- [ ] **Step 1: Write the failing assertion**

Establish the baseline first, so that a later failure is attributable. Build and boot any existing system on the current machine type and save the log:

```bash
cd /home/esma/workspace/composite_main
./cos init x86_64
./cos build
./cos compose composition_scripts/unit_heap.toml baseline
./cos run baseline 2>&1 | tee /tmp/boot-baseline.log
grep -c "SUCCESS" /tmp/boot-baseline.log
```

Record what a healthy boot looks like. The assertion for this task is that the same system boots to the same `SUCCESS` lines with the IOMMU machine type.

- [ ] **Step 2: Run it to verify it fails**

```bash
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
```

Expected: failure — `run.sh` does not understand an `iommu` argument yet, so it is silently ignored and the boot is identical to baseline. Confirm by checking QEMU's own view:

```bash
grep -c "q35" /tmp/boot-q35.log   # expected: 0
```

- [ ] **Step 3: Implement the machine-type option**

In `tools/run.sh`, the x86_64 branch currently reads:

```bash
qemu-system-x86_64 ${kvm_flag} -cpu max -smp ${vcpus},cores=${num_cores},threads=${num_threads},sockets=${num_sockets} -m ${mem_size} -cdrom $1 -no-reboot -nographic -s ${debug_flag} -nic none ${nic_flag}
```

Add an option that sets a machine flag, alongside the existing `debug` and `enable-nic` handling:

```bash
machine_flag=""
if [ "${debug_flag}" == "iommu" ] || [ "${nic_flag}" == "iommu" ]
then
	if [ "${debug_flag}" == "iommu" ]; then debug_flag=""; fi
	if [ "${nic_flag}" == "iommu" ]; then nic_flag=""; fi
	machine_flag=" -M q35,kernel-irqchip=split -device intel-iommu "
fi
```

and insert `${machine_flag}` into the `qemu-system-x86_64` line.

Note for the implementer: `kernel-irqchip=split` is required by QEMU for `intel-iommu` and is not optional. Interrupt remapping stays off for Phase 1; if QEMU's default for `intremap` causes trouble, set it explicitly to `off`.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
grep -c "SUCCESS" /tmp/boot-q35.log
```

Expected: the same `SUCCESS` count as `/tmp/boot-baseline.log`.

**If Composite does not boot on `q35`, stop and report rather than working around it.** This is a genuine finding, not an obstacle to route past: it would mean the test environment for the whole feature needs rethinking, and it changes the plan. Capture where the boot stops.

- [ ] **Step 5: Commit**

```bash
git add tools/run.sh
git commit -m "tools: add q35 + intel-iommu QEMU machine option to run.sh"
```

---

### Task 2: XSDT support in the ACPI parser

`miniacpi.c` walks only the RSDT, with 32-bit table pointers. Modern firmware may publish no RSDT at all, in which case every table lookup — including the DMAR lookup Task 3 depends on — fails. This task is the hard prerequisite the spec identifies as K1.

**Files:**
- Modify: `src/platform/x86_64/miniacpi.c:39` (the `rsdt` global), `:41-74` (`acpi_find_rsdt`), `:99-121` (`acpi_iterate_tbs`), `:124-146` (`acpi_find_resource_flags`), `:339` (`acpi_init`)

**Interfaces:**
- Consumes: nothing.
- Produces: `acpi_find_resource(const char *sig)` — unchanged signature, now working against either root table. Task 3 calls it with `"DMAR"`.

- [ ] **Step 1: Write the failing assertion**

The kernel must report which root table it is using and how many entries it holds. Assert on that line:

```bash
grep -E "ACPI: root table is (RSDT|XSDT) with [0-9]+ entries" /tmp/boot-q35.log
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
grep -E "ACPI: root table is (RSDT|XSDT)" /tmp/boot-q35.log
```

Expected: no match. The current parser prints `RSDT vaddr is @ %p` and nothing about which flavour it chose.

- [ ] **Step 3: Implement entry-width abstraction and XSDT selection**

The two root tables differ only in entry width, so introduce accessors and let both callers use them rather than duplicating the walk. In `miniacpi.c`, replace the bare `static struct rsdt *rsdt;` at line 39:

```c
/*
 * The ACPI root table, which is either an RSDT (32-bit entry pointers)
 * or an XSDT (64-bit).  ACPI 2.0+ firmware may publish only the XSDT,
 * so we must handle both.  The two are identical apart from entry
 * width, hence the accessors below rather than two copies of the walk.
 */
static struct rsdt *rsdt;
static int          rsdt_is_xsdt;

static size_t
acpi_sdt_entries(void)
{
	size_t entsz = rsdt_is_xsdt ? sizeof(u64_t) : sizeof(u32_t);

	return (rsdt->head.len - sizeof(struct rsdt)) / entsz;
}

static paddr_t
acpi_sdt_entry(size_t i)
{
	if (rsdt_is_xsdt) return (paddr_t)(((u64_t *)rsdt->entry)[i]);

	return (paddr_t)(((u32_t *)rsdt->entry)[i]);
}
```

`sizeof(struct rsdt)` is 36, the ACPI system-description-table header size, because `entry[0]` is a flexible array member. That is the correct divisor for both flavours.

In `acpi_find_rsdt`, prefer the XSDT when the RSDP revision offers one:

```c
	if (!rsdp) return NULL;

	if (rsdp->revision >= 2 && rsdp->xsdtaddress) {
		rsdt_is_xsdt = 1;
		return device_map_mem((paddr_t)rsdp->xsdtaddress, 0);
	}
	rsdt_is_xsdt = 0;
	return device_map_mem((paddr_t)rsdp->rsdtaddress, 0);
```

Fix the checksum loop in the same function while you are in it. It sums `r->length` bytes unconditionally, but `length` only exists in revision 2 and later; for a revision 0 RSDP the field is beyond the structure and the sum reads garbage:

```c
			struct rsdp * r   = (struct rsdp *)sig;
			unsigned char sum = 0;
			u32_t         i;
			/* Revision 0 RSDPs are 20 bytes and have no length field. */
			u32_t         len = (r->revision >= 2) ? r->length : 20;

			for (i = 0; i < len; i++) {
				sum += sig[i];
			}
```

Then convert both walkers to the accessors. In `acpi_iterate_tbs`:

```c
	size_t entries = acpi_sdt_entries();

	printk("ACPI: root table is %s with %u entries\n",
	       rsdt_is_xsdt ? "XSDT" : "RSDT", (unsigned)entries);
	for (i = 0; i < entries; i++) {
		paddr_t      pa = acpi_sdt_entry(i);
		struct rsdt *e  = (struct rsdt *)device_pa2va(pa);

		if (!e) {
			e = device_map_mem(pa, 0);
			assert(e);
		}

		memcpy(name, e->head.sig, SDT_NAME_SZ - 1);
		printk("\t%s\n", name);
	}
```

and identically in `acpi_find_resource_flags`, replacing its `entries` computation and its `rsdt->entry[i]` uses with `acpi_sdt_entries()` and `acpi_sdt_entry(i)`, keeping its existing `flags` argument on the `device_map_mem` call.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
grep -E "ACPI: root table is (RSDT|XSDT) with [0-9]+ entries" /tmp/boot-q35.log
grep "DMAR" /tmp/boot-q35.log
```

Expected: the root-table line appears, and `DMAR` is among the listed table signatures. The second grep is the real point of this task — it proves the DMAR table is reachable before Task 3 tries to parse it.

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/miniacpi.c
git commit -m "x86_64/acpi: walk the XSDT when firmware provides one

Also fixes the RSDP checksum to respect revision: the length field
only exists in revision 2 and later, so revision 0 RSDPs were summed
over garbage."
```

---

### Task 3: DMAR table parse

**Files:**
- Create: `src/platform/x86_64/dmar.h`, `src/platform/x86_64/dmar.c`
- Modify: `src/platform/x86_64/Makefile` (add `dmar.o`), `src/platform/x86_64/kernel.c:175` (call `dmar_init()` after `acpi_init()`)

**Interfaces:**
- Consumes: `acpi_find_resource("DMAR")` from Task 2.
- Produces:
  - `void dmar_init(void)` — parses the table, populates the unit array, prints what it found. Safe to call when no DMAR table exists.
  - `unsigned dmar_unit_count(void)` — number of parsed DRHD units.
  - `struct dmar_unit *dmar_unit_get(unsigned i)` — the parsed unit, or NULL.
  - `int dmar_rmrr_covers(u16_t bdf)` — non-zero if the device is named in any RMRR scope. Phase 1 refuses to bind such devices, per the spec.

- [ ] **Step 1: Write the failing assertion**

```bash
grep -E "DMAR: [0-9]+ remapping unit\(s\), host address width [0-9]+" /tmp/boot-q35.log
grep -E "DMAR: unit [0-9]+ regs @ 0x[0-9a-f]+" /tmp/boot-q35.log
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
grep "DMAR:" /tmp/boot-q35.log
```

Expected: no match. Nothing parses the table yet.

- [ ] **Step 3: Implement the parse**

Create `src/platform/x86_64/dmar.h`:

```c
#ifndef DMAR_H
#define DMAR_H

#include "shared/cos_types.h"

/*
 * Fail fast rather than silently truncating: a machine with more
 * remapping units or reserved regions than we planned for is a
 * configuration we have not reasoned about.
 */
#define DMAR_UNIT_MAX 8
#define DMAR_RMRR_MAX 16

struct dmar_unit {
	paddr_t  reg_base;    /* physical base of this unit's register set */
	void    *regs;        /* mapped virtual address; NULL until Task 4 */
	u16_t    segment;     /* PCI segment this unit covers              */
	u8_t     include_all; /* unit covers every device not claimed      */
};

struct dmar_rmrr {
	u16_t   segment;
	paddr_t base;
	paddr_t limit;
};

void              dmar_init(void);
unsigned          dmar_unit_count(void);
struct dmar_unit *dmar_unit_get(unsigned i);
int               dmar_rmrr_covers(u16_t bdf);

#endif /* DMAR_H */
```

Create `src/platform/x86_64/dmar.c`. The DMAR ACPI table is a standard 36-byte system-description-table header, then a one-byte host address width, a one-byte flags field, ten reserved bytes, then a sequence of variable-length remapping structures each carrying a 2-byte type and a 2-byte length. Walk that sequence by length, dispatching on type; DRHD and RMRR are the two types Phase 1 cares about, and unknown types are skipped by their length rather than treated as errors, since firmware may legitimately report structures we do not handle.

**Source the numeric type codes, the DRHD and RMRR structure field offsets, and the device-scope entry layout from the VT-d specification.** Name them as `#define`s at the top of the file so a reviewer can check them against the spec in one place rather than hunting through the walk. Structure the walk as:

```c
static struct dmar_unit units[DMAR_UNIT_MAX];
static unsigned         unit_cnt;
static struct dmar_rmrr rmrrs[DMAR_RMRR_MAX];
static unsigned         rmrr_cnt;

void
dmar_init(void)
{
	void *tbl = acpi_find_resource("DMAR");
	u8_t  host_addr_width;
	unsigned i;

	if (!tbl) {
		printk("DMAR: no table found; IOMMU unavailable\n");
		return;
	}

	/* walk the remapping structures, filling units[] and rmrrs[] */

	printk("DMAR: %u remapping unit(s), host address width %u\n",
	       unit_cnt, host_addr_width);
	for (i = 0; i < unit_cnt; i++) {
		printk("DMAR: unit %u regs @ 0x%llx segment %u%s\n",
		       i, units[i].reg_base, units[i].segment,
		       units[i].include_all ? " include-all" : "");
	}
	for (i = 0; i < rmrr_cnt; i++) {
		printk("DMAR: rmrr [0x%llx, 0x%llx] segment %u\n",
		       rmrrs[i].base, rmrrs[i].limit, rmrrs[i].segment);
	}
}
```

Assert on overflow of either array rather than truncating.

In `kernel.c`, call it immediately after `acpi_init()` at line 175, since it depends on the ACPI table lookup being live:

```c
	acpi_init();
	dmar_init();
	lapic_init();
```

Add `dmar.o` to the platform `Makefile` object list beside `miniacpi.o`.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
grep "DMAR:" /tmp/boot-q35.log
```

Expected under QEMU `-device intel-iommu`: exactly one unit, an include-all flag, and typically no RMRRs. Also confirm the no-IOMMU path is clean, since most developers will run without the flag:

```bash
./cos run baseline 2>&1 | grep "DMAR: no table found"
```

Expected: that line, and a boot that otherwise completes normally.

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/dmar.h src/platform/x86_64/dmar.c \
        src/platform/x86_64/Makefile src/platform/x86_64/kernel.c
git commit -m "x86_64/dmar: parse the ACPI DMAR table

Records DRHD units and RMRR regions into bounded static state and
reports them at boot.  No hardware is touched yet."
```

---

### Task 4: Map DMAR registers and decode capabilities

This task produces the facts the second plan depends on: how many domains the hardware supports, which address widths, whether queued invalidation exists, and — the one that changes code structure — whether the unit is page-walk coherent.

**Files:**
- Modify: `src/platform/x86_64/dmar.c`, `src/platform/x86_64/dmar.h`

**Interfaces:**
- Consumes: `struct dmar_unit` from Task 3.
- Produces:
  - `struct dmar_unit` gains `regs` (mapped), plus decoded fields: `u8_t max_addr_width`, `u16_t num_domains`, `u8_t qi_supported`, `u8_t coherent`.
  - `int dmar_hw_usable(void)` — non-zero when every parsed unit supports what the design requires: queued invalidation, and an address width covering the machine's physical range.

- [ ] **Step 1: Write the failing assertion**

```bash
grep -E "DMAR: unit 0 version [0-9]+\.[0-9]+ domains [0-9]+ addr-width [0-9]+ qi (yes|no) coherent (yes|no)" /tmp/boot-q35.log
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./cos run baseline iommu 2>&1 | grep "coherent"
```

Expected: no match. Task 3 prints the register base but never reads through it.

- [ ] **Step 3: Implement mapping and decode**

Map each unit's register set using the existing kernel device-mapping path, the same one ACPI uses:

```c
		units[i].regs = device_map_mem(units[i].reg_base, PGTBL_NOCACHE);
		assert(units[i].regs);
```

`PGTBL_NOCACHE` matters — these are device registers, and `acpi_find_apic` in `miniacpi.c` already uses that flag for the same reason.

Then read the version, capability, and extended capability registers and decode them into the `struct dmar_unit` fields. **Take the register offsets and every capability bit position and field width from the VT-d specification.** Define them as named constants at the top of `dmar.c`. The fields to extract are: major and minor version; number of supported domains; maximum guest address width; queued-invalidation support; and page-walk coherency.

Report each unit:

```c
	printk("DMAR: unit %u version %u.%u domains %u addr-width %u qi %s coherent %s\n",
	       i, ver_major, ver_minor, units[i].num_domains,
	       units[i].max_addr_width,
	       units[i].qi_supported ? "yes" : "no",
	       units[i].coherent ? "yes" : "no");
```

Implement `dmar_hw_usable()` to return zero, with an explanatory printk, if any unit lacks queued invalidation. The lazy-invalidation design in the spec assumes a queue; a machine without one needs a different design and should say so loudly at boot rather than fail mysteriously later.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
./cos run baseline iommu 2>&1 | tee /tmp/boot-q35.log
grep "DMAR: unit" /tmp/boot-q35.log
```

Expected: a decoded line per unit.

**Cross-check the decode against a known-good source rather than trusting it.** The A03 target runs Linux, which parses the same registers on the same silicon. Compare:

```bash
ssh syslab@161.253.78.154 'dmesg | grep -i dmar; ls /sys/class/iommu/'
```

If the kernel's reported domain count, address width, and coherency disagree with Linux's on the same machine, the decode is wrong — that is precisely the failure mode a fabricated bit offset produces, and it is the reason this cross-check is a step rather than a suggestion.

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/dmar.c src/platform/x86_64/dmar.h
git commit -m "x86_64/dmar: map unit registers and decode capabilities

Reports version, domain count, address width, queued-invalidation
support and page-walk coherency per unit.  dmar_hw_usable() rejects
hardware the lazy-invalidation design cannot run on."
```

---

### Task 5: `PGTBL_TYPE_IOMMU` page-table type

The last foundation piece: a capability-typed page table in the VT-d second-level format, allocatable the same way an EPT root already is.

**Files:**
- Modify: `src/platform/x86_64/chal/chal_proto.h:9-12`, `src/platform/x86_64/chal_pgtbl.h:50`, `src/platform/x86_64/chal_pgtbl.c:248-253`, `:730`, `:772`
- Modify: `src/components/lib/kernel/cos_kernel_api.h:103`
- Create: `src/components/implementation/tests/unit_iommu/` (component + `Makefile`), `composition_scripts/unit_iommu.toml`

**Interfaces:**
- Consumes: nothing from Tasks 1–4; this is independent of the hardware discovery and can be reviewed separately.
- Produces: `cos_pgtbl_alloc(ci, PGTBL_TYPE_IOMMU)` returns a valid page-table capability whose `struct cap_pgtbl.type` is `PGTBL_TYPE_IOMMU`. Task 6 of the next plan binds a device to one.

- [ ] **Step 1: Write the failing test**

Create `src/components/implementation/tests/unit_iommu/iommu.c`, following the `unit_memmgr` convention of printing `SUCCESS:`/`FAILURE:` lines:

```c
#include <cos_types.h>
#include <cos_kernel_api.h>
#include <cos_component.h>
#include <cos_defkernel_api.h>
#include <llprint.h>

static void
test_iommu_pgtbl_alloc(void)
{
	struct cos_compinfo *ci = cos_compinfo_get(cos_defcompinfo_curr_get());
	pgtblcap_t           pt;

	pt = cos_pgtbl_alloc(ci, PGTBL_TYPE_IOMMU);
	if (!pt) {
		printc("FAILURE: could not allocate an IOMMU-typed page table\n");
		return;
	}
	printc("SUCCESS: allocated IOMMU-typed page table cap %ld\n", (long)pt);
}

int
main(void)
{
	test_iommu_pgtbl_alloc();
	return 0;
}
```

Its `Makefile`, following `unit_memmgr`'s exactly:

```make
INTERFACE_EXPORTS =
INTERFACE_DEPENDENCIES = init memmgr
LIBRARY_DEPENDENCIES =

include Makefile.subsubdir
```

And `composition_scripts/unit_iommu.toml`, copied from **`unit_heap.toml`** with the final component's `name` and `img` changed to `unit_iommu` and `tests.unit_iommu`. Do not copy `unit_memmgr.toml` — it names a scheduler that does not exist in this tree and will not compose.

- [ ] **Step 2: Run it to verify it fails**

```bash
./cos build && ./cos compose composition_scripts/unit_iommu.toml iommutest
```

Expected: a compile failure — `PGTBL_TYPE_IOMMU` is undefined. That is the correct first failure.

- [ ] **Step 3: Implement the type**

In `src/platform/x86_64/chal/chal_proto.h`, beside the existing types at line 9:

```c
#define PGTBL_TYPE_DEF (0)
#define PGTBL_TYPE_EPT (1)
#define PGTBL_TYPE_IOMMU (2)
```

Mirror the constant in `src/components/lib/kernel/cos_kernel_api.h`, which carries its own copy at line 103, and add the matching level flag beside `PGTBL_LVL_FLAG_VM` at line 104:

```c
#define PGTBL_TYPE_IOMMU (2)
#define PGTBL_LVL_FLAG_VM (1UL << 31)
#define PGTBL_LVL_FLAG_IOMMU (1UL << 30)
```

**Then fix `cos_pgtbl_alloc`, which is the trap in this task.** At `cos_kernel_api.c:1213` it reads:

```c
	if (unlikely(type)) lvl |= PGTBL_LVL_FLAG_VM;
```

It treats `type` as a boolean. Since `PGTBL_TYPE_IOMMU` is `2` and therefore truthy, an unfixed `cos_pgtbl_alloc(ci, PGTBL_TYPE_IOMMU)` would set the *EPT* flag and hand back a perfectly valid EPT page table — and the test in Step 1 would pass while the feature did not work. Map the type explicitly:

```c
	if (type == PGTBL_TYPE_EPT)        lvl |= PGTBL_LVL_FLAG_VM;
	else if (type == PGTBL_TYPE_IOMMU) lvl |= PGTBL_LVL_FLAG_IOMMU;
```

The existing type is smuggled through the page-table *level* argument via `PGTBL_FLAG_EPT (0x80000000UL)` and its mask — a single bit, which cannot express a third type. Extend it to a two-bit field, keeping the existing EPT encoding so no current caller changes meaning:

```c
/*
 * The page-table type is passed to activation packed into the high
 * bits of the level argument.  Two bits, so DEF/EPT/IOMMU all fit.
 * EPT keeps bit 31 -- its value when this was a single flag -- because
 * user-level names that same bit PGTBL_LVL_FLAG_VM and passes it
 * independently.  Changing it would silently mistype every VM page
 * table in the system.
 */
#define PGTBL_FLAG_TYPE_SHIFT (30)
#define PGTBL_FLAG_TYPE_MASK  (0x3UL << PGTBL_FLAG_TYPE_SHIFT) /* 0xC0000000 */
#define PGTBL_FLAG_EPT        (0x2UL << PGTBL_FLAG_TYPE_SHIFT) /* 0x80000000, unchanged */
#define PGTBL_FLAG_IOMMU      (0x1UL << PGTBL_FLAG_TYPE_SHIFT) /* 0x40000000 */
#define PGTBL_FLAG_EPT_MASK   (~PGTBL_FLAG_TYPE_MASK)
```

Check the arithmetic before moving on: `0x2UL << 30` is `0x80000000`, matching both the old `PGTBL_FLAG_EPT` and user-level's `PGTBL_LVL_FLAG_VM (1UL << 31)` in `cos_kernel_api.h:104`. If those three ever disagree, EPT page tables get built as the wrong type.

In `chal_pgtbl.c` around line 248, the activation currently branches on the single flag:

```c
	pt->lvl          = lvl & PGTBL_FLAG_EPT_MASK;

	if (unlikely(lvl & PGTBL_FLAG_EPT)) {
		pt->type = PGTBL_TYPE_EPT;
	} else {
		pt->type = PGTBL_TYPE_DEF;
	}
```

Replace the branch with a decode of the two-bit field, mapping `PGTBL_FLAG_IOMMU` to `PGTBL_TYPE_IOMMU` and preserving the other two cases exactly.

In `chal_pgtbl.h`, add VT-d second-level flag definitions beside the EPT block at line 50. VT-d second-level entries carry read, write, and execute permissions in low bits, and a snoop control bit; **take the exact positions from the VT-d specification** and define a `x86_VTD_SL_VM_DEF` analogous to the existing `x86_EPT_VM_DEF`.

At `chal_pgtbl.c:730` and `:772`, the type is compared against `PGTBL_TYPE_EPT` and `PGTBL_TYPE_DEF` to select flag handling. Add the `PGTBL_TYPE_IOMMU` case to both, using the new VT-d flag set.

Finally, make the type observable. User level cannot read a page table's type back — `CAPTBL_OP_INTROSPECT` on a page table resolves an address, not a type — so a component-side test can only prove that *some* capability was returned. Print the type where it is assigned, in the activation path near `chal_pgtbl.c:248`:

```c
	printk("pgtbl activate: type %u lvl %u\n", pt->type, pt->lvl);
```

This is the assertion that distinguishes a working feature from the boolean trap above, and it is worth keeping permanently.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_iommu.toml iommutest
./cos run iommutest 2>&1 | tee /tmp/boot-iommutest.log
grep "SUCCESS: allocated IOMMU-typed page table" /tmp/boot-iommutest.log
grep "pgtbl activate: type 2" /tmp/boot-iommutest.log
```

Expected: both lines. The second is the one that matters — it proves the kernel built a type-2 table rather than silently falling back to EPT.

Then confirm nothing regressed for the existing types — this task touched an encoding every page table in the system flows through, so a passing new test is not sufficient evidence:

```bash
./cos compose composition_scripts/unit_heap.toml baseline
./cos run baseline 2>&1 | grep -c SUCCESS
./cos compose composition_scripts/simple_vmm.toml vmmtest
./cos run vmmtest 2>&1 | tail -40
```

Expected: the `unit_memmgr` `SUCCESS` count matches the Task 1 baseline, and the VMM still reaches its guest-boot output. The VMM check is the important one: it is the only existing consumer of `PGTBL_TYPE_EPT`, and therefore the only thing that would notice a botched re-encoding.

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/chal/chal_proto.h src/platform/x86_64/chal_pgtbl.h \
        src/platform/x86_64/chal_pgtbl.c src/components/lib/kernel/cos_kernel_api.h \
        src/components/implementation/tests/unit_iommu composition_scripts/unit_iommu.toml
git commit -m "x86_64/pgtbl: add PGTBL_TYPE_IOMMU as a third page-table type

Widens the type encoding in the activation level argument from one bit
to two, preserving the existing EPT value, and adds VT-d second-level
flag translation beside the EPT set."
```

---

## Outcome of Task 5's test, and what replaced it

The component test drafted for Task 5 does not work and was removed. `cos_pgtbl_alloc`
reaches `__mem_bump_alloc`, which asserts on the meta-capability: allocating a page
table requires untyped memory, which only a component holding the boot untyped page
table has. An ordinary test component gets memory through `memmgr` and cannot mint page
tables at all. The one existing caller in the tree, `tests/unit_defcompinfo`, works by
running as the booter, and no composition script builds it.

Evidence for Task 5 is therefore: `COS_STATIC_ASSERT`s pinning the type encoding
(verified to fire by changing the shift and watching the build fail), and a regression
run showing `simple_vmm` still creates EPT page tables and reaches the same milestones
as the pre-change baseline. Creating an IOMMU-typed table is left to the next plan's
K5/K6, where the kernel does it — which is how the feature will actually be used.

## What this plan establishes for the next one

On completion the kernel discovers the IOMMU, reports its capabilities, and can allocate IOMMU-typed page tables. The four open questions from the scope section will have answers: whether `q35` boots, which root table the firmware publishes, whether the units are coherent, and how many there are on the two-socket target.

The second plan then covers root and context tables (K5), the bind operation (K6), the mirroring hook (K7), the invalidation queue (K8), the quiescence extension (K9), polled fault recording (K10), and the userlevel work (U1–U4). K7 and K9 are the two the spec already flags as most likely to change under scrutiny, and both are better designed against measured hardware behaviour than against assumptions.
