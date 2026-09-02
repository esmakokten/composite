# IOMMU (VT-d) Phase 1, Translation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the IOMMU on — root and context tables, queued invalidation, translation enabled — and make a device's DMA actually go through it, with faults visible.

**Architecture:** Build the root and context tables the hardware walks to find a domain, program them through the registers `dmar.c` already owns, and enable translation. Devices start in pass-through so that enabling translation is provably harmless; only then is a device moved to a real domain, where a blocked DMA must produce a recorded fault.

**Tech Stack:** C (kernel, `src/platform/x86_64/dmar.c`), the VT-d register set, `PGTBL_TYPE_IOMMU` page tables from the foundation plan, QEMU `q35` + `intel-iommu`, Dell R740.

**Spec:** `doc/specs/2026-09-01-iommu-vtd-design.md`
**Predecessor:** `doc/plans/2026-09-01-iommu-vtd-phase1-foundation.md` (K1–K4, complete)

## Scope

Covers **K5** (root and context tables), **K6** (bind operation), **K8** (invalidation queue) and **K10** (polled fault recording). Deliberately excludes **K7** (the mirroring hook) and **K9** (the quiescence extension), which are the two the spec flags as most likely to change under scrutiny and which both depend on a working bind existing first. They get their own plan.

The milestone is a falsifiable containment result: a device assigned to an empty domain attempts DMA, and the hardware records a fault instead of the memory being touched.

## Global Constraints

Inherited from the foundation plan, and still binding:

- **`src/platform/x86_64/` is mostly symlinks into `src/platform/i386/`.** `chal_pgtbl.c`, `chal_pgtbl.h`, `chal/chal_proto.h`, `kernel.c` and `miniacpi.c` are shared. `dmar.c`, `dmar.h` and `Makefile` are x86_64's own. Run `readlink` before editing a platform file.
- **i386 is not a supported target for this work** and is not verified; keep the `__x86_64__` guards on shared-file call sites.
- **Hardware constants come from the VT-d specification, not from this document.** Register offsets, descriptor layouts and bit positions are named here by their spec names. The foundation plan's practice holds: print raw values beside the decode and check them by hand.

New, and load-bearing for every task here:

- **Every write to a remapping structure must be flushed when the unit is not page-walk coherent.** QEMU reports `ECAP.C = 0`, the R740 reports `1`. A `dmar_flush_cache()` helper conditional on the unit's `coherent` field must wrap every root, context, and page-table write. Getting this wrong fails only on QEMU, and silently.
- **Four units on the R740, one on QEMU.** Anything programmed must be programmed per unit, and any invalidation must go to the unit that owns the device — `dmar_unit_for_bdf()` exists for this.
- **Never enable translation without a valid root table already installed and invalidated.** The ordering in each task is not stylistic.

## The verification loop

```bash
cd /home/esma/workspace/composite_main
./cos build
./cos compose composition_scripts/unit_heap.toml baseline
timeout 40 ./cos run baseline iommu 2>&1 | tee /tmp/boot.log   # q35 + intel-iommu
timeout 35 ./cos run baseline          2>&1 | tee /tmp/boot-pc.log  # no IOMMU, must stay clean
```

Kernel tasks assert on specific `printk` lines. **Both** runs matter on every task: the no-IOMMU path must keep degrading gracefully, and it is the one a developer without the flag will hit.

## File structure

| File | Responsibility | Status |
|---|---|---|
| `src/platform/x86_64/dmar.h` | Unit state, domain handle, public interface | Modify |
| `src/platform/x86_64/dmar.c` | Root/context tables, invalidation queue, translation enable, faults | Modify |
| `src/kernel/include/shared/cos_types.h` | New capability operation for device binding | Modify |
| `src/kernel/capinv.c` | Dispatch for the bind operation | Modify |

`dmar.c` grows substantially in this plan. If it passes roughly 600 lines, split the invalidation queue into `dmar_qi.c` — it has a clean boundary (submit a descriptor, wait for completion) and no other part of the file needs its internals.

---

### Task 1: Cache-flush helper and the invalidation queue

Invalidation comes first because every later task needs it: a context entry written without invalidating the context cache may simply not be seen.

**Files:**
- Modify: `src/platform/x86_64/dmar.c`, `src/platform/x86_64/dmar.h`

**Interfaces:**
- Consumes: `struct dmar_unit` with `regs`, `coherent`, `qi_supported` from the foundation plan.
- Produces:
  - `void dmar_flush_cache(struct dmar_unit *u, void *addr, size_t sz)` — no-op when the unit is coherent, `clflush` over the range otherwise.
  - `int dmar_qi_init(struct dmar_unit *u)` — allocate the queue, program the queue address register, enable queued invalidation.
  - `int dmar_qi_submit_wait(struct dmar_unit *u, u64_t d0, u64_t d1)` — submit one descriptor followed by an invalidation-wait descriptor, and spin until the wait's status write lands. Returns non-zero on timeout.
  - `int dmar_inv_context_global(struct dmar_unit *u)` and `int dmar_inv_iotlb_global(struct dmar_unit *u)`.

- [ ] **Step 1: Write the failing assertion**

```bash
grep -E "DMAR: unit 0 queued invalidation enabled" /tmp/boot.log
grep -E "DMAR: unit 0 global context\+iotlb invalidation ok" /tmp/boot.log
```

- [ ] **Step 2: Run to verify it fails**

```bash
timeout 40 ./cos run baseline iommu 2>&1 | tee /tmp/boot.log
grep -c "queued invalidation enabled" /tmp/boot.log     # expected: 0
```

- [ ] **Step 3: Implement**

The invalidation queue is a page of 128-bit descriptors. Allocation must come from memory the kernel can hand the hardware a physical address for; follow how `dmar.c` will allocate the root table in Task 2 and use the same source, so there is one answer to "where do remapping structures live" rather than two.

Write `dmar_flush_cache` first and use it everywhere:

```c
static void
dmar_flush_cache(struct dmar_unit *u, void *addr, size_t sz)
{
	char *p;

	/* A coherent unit reads what the CPU wrote; nothing to do. */
	if (u->coherent) return;
	for (p = (char *)addr; p < (char *)addr + sz; p += CACHE_LINE) {
		__asm__ volatile("clflush (%0)" : : "r"(p) : "memory");
	}
	__asm__ volatile("sfence" ::: "memory");
}
```

Then `dmar_qi_init`: zero the queue page, flush it, write the queue address register, clear the head and tail registers, and set the queued-invalidation-enable bit in the global command register — then poll global status until the hardware acknowledges. **Every global command register write is read-modify-write against global status, and must be followed by a poll for the corresponding status bit.** The register is one-shot per command; blindly writing a whole word clobbers other enables.

`dmar_qi_submit_wait` writes the caller's descriptor at the tail, then an invalidation-wait descriptor whose status-write field points at a kernel `volatile u32_t`, advances the tail register, and spins on that word changing. Bound the spin and return non-zero rather than hanging: a wedged queue must be a reported failure, not a dead machine.

Report once per unit, and exercise both invalidations at init so the assertion above has something to check.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
timeout 40 ./cos run baseline iommu 2>&1 | tee /tmp/boot.log
grep -E "queued invalidation enabled|global context\+iotlb invalidation ok" /tmp/boot.log
timeout 35 ./cos run baseline 2>&1 | grep "DMAR: no table found"
```

Expected: both lines on q35; the no-IOMMU boot unchanged.

Then prove the timeout path is real rather than theoretical, since it is the only thing standing between a queue bug and a hung machine: temporarily submit a descriptor without advancing the tail register, confirm `dmar_qi_submit_wait` returns non-zero and the boot continues, then revert.

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/dmar.c src/platform/x86_64/dmar.h
git commit -m "x86_64/dmar: queued invalidation and coherency-aware cache flushing"
```

---

### Task 2: Root and context tables, translation enabled in pass-through

**Files:**
- Modify: `src/platform/x86_64/dmar.c`, `src/platform/x86_64/dmar.h`

**Interfaces:**
- Consumes: Task 1's invalidation and flush helpers.
- Produces:
  - `int dmar_translation_enable(struct dmar_unit *u)` — install the root table, invalidate, set the translation-enable bit, poll for it.
  - Every device defaults to a pass-through context entry, so enabling translation changes no device's behaviour.

- [ ] **Step 1: Write the failing assertion**

```bash
grep -E "DMAR: unit 0 translation enabled \(pass-through default\)" /tmp/boot.log
```

and, the assertion that actually matters, that the machine is unharmed:

```bash
grep -c "adding@" /tmp/boot.log     # unit_heap still running, comparable to before
```

- [ ] **Step 2: Run to verify it fails**

```bash
timeout 40 ./cos run baseline iommu 2>&1 | grep -c "translation enabled"   # expected: 0
```

- [ ] **Step 3: Implement**

The root table is one page, indexed by bus number, each entry pointing at a context table for that bus. A context table is one page, indexed by device and function, each entry naming a domain: a translation type, a second-level page table pointer, an address width, and a domain id.

Allocate lazily — a context table per bus only when a device on that bus is first programmed — rather than 256 pages up front.

Set every device's context entry to **pass-through** translation type. Pass-through is supported on both targets (`ECAP.PT`, bit 6, is set on QEMU and the R740), and it means the device addresses memory exactly as it does today. Enabling translation therefore proves the tables and register programming are right while changing nothing observable, which is the point of doing it in this order.

Ordering, which is not negotiable: write the context and root tables, `dmar_flush_cache` them, write the root table address register, invalidate context cache and IOTLB globally, and only then set translation-enable and poll global status.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
timeout 40 ./cos run baseline iommu 2>&1 | tee /tmp/boot.log
grep "translation enabled" /tmp/boot.log
grep -c "adding@" /tmp/boot.log
```

Expected: translation enabled, and the heap test running at a rate comparable to the foundation plan's baseline. **A boot that hangs or loses the console here means a device Composite depends on stopped working** — capture where it stopped rather than retrying.

Also run the VMM composition, which is the heaviest DMA-free workload available, to confirm nothing else regressed:

```bash
./cos compose composition_scripts/simple_vmm.toml vmmtest
timeout 60 ./cos run vmmtest iommu 2>&1 | grep -E "created VM with|has been loaded"
```

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/dmar.c src/platform/x86_64/dmar.h
git commit -m "x86_64/dmar: root/context tables and translation enable, pass-through default"
```

---

### Task 3: Polled fault recording

Without this, a blocked DMA in Task 4 is indistinguishable from a device that simply did nothing. Fault reporting by interrupt needs MSI, which is Phase 2; polling is what Phase 1 gets.

**Files:**
- Modify: `src/platform/x86_64/dmar.c`, `src/platform/x86_64/dmar.h`

**Interfaces:**
- Consumes: mapped unit registers.
- Produces:
  - `int dmar_fault_poll(struct dmar_unit *u)` — read and clear pending fault recording registers, printing each; returns the number of faults consumed.

- [ ] **Step 1: Write the failing assertion**

Faults cannot be asserted on until Task 4 causes one, so this task's assertion is that polling runs and reports nothing on a healthy boot:

```bash
grep -E "DMAR: unit 0 fault status clean" /tmp/boot.log
```

- [ ] **Step 2: Run to verify it fails**

```bash
timeout 40 ./cos run baseline iommu 2>&1 | grep -c "fault status"    # expected: 0
```

- [ ] **Step 3: Implement**

The number of fault recording registers and their offset both come from the capability register — the foundation plan already reads `CAP`, so extend its decode rather than re-reading. Each fault record carries the faulting address, the source id, the fault reason and a read/write indicator; a valid bit says whether the record is populated, and is cleared by writing it back.

Print faults in a form that names the device, since "a fault happened" is not actionable:

```c
printk("DMAR: fault %02x:%02x.%u %s @ 0x%lx reason 0x%x\n", ...);
```

Call it once at the end of `dmar_init` for the clean-boot assertion.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
timeout 40 ./cos run baseline iommu 2>&1 | grep "fault status clean"
```

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/dmar.c src/platform/x86_64/dmar.h
git commit -m "x86_64/dmar: polled fault recording"
```

---

### Task 4: Domain binding, and the containment demonstration

**Files:**
- Modify: `src/platform/x86_64/dmar.c`, `src/platform/x86_64/dmar.h`, `src/kernel/include/shared/cos_types.h`, `src/kernel/capinv.c`

**Interfaces:**
- Consumes: Tasks 1–3, and `PGTBL_TYPE_IOMMU` from the foundation plan.
- Produces:
  - `int dmar_domain_bind(u16_t bdf, pgtbl_t sl_root, u8_t aw)` — allocate a domain id, write the device's context entry to second-level translation against `sl_root`, flush, invalidate that unit's context cache and IOTLB.
  - `CAPTBL_OP_IOMMU_BIND` — the capability operation reaching it, validating that the page table capability is `PGTBL_TYPE_IOMMU` and that the device carries no RMRR (`dmar_rmrr_covers`), refusing otherwise.

- [ ] **Step 1: Write the failing assertion**

This is the milestone's real test, and it is a falsification experiment. A device is bound to an IOMMU-typed page table that maps nothing; any DMA it attempts must fault.

```bash
grep -E "DMAR: bound [0-9a-f]{2}:[0-9a-f]{2}\.[0-9] to domain [0-9]+" /tmp/boot.log
grep -E "DMAR: fault [0-9a-f]{2}:[0-9a-f]{2}\.[0-9] .* reason" /tmp/boot.log
```

- [ ] **Step 2: Run to verify it fails**

```bash
timeout 40 ./cos run baseline iommu 2>&1 | grep -c "bound"     # expected: 0
```

- [ ] **Step 3: Implement**

Domain ids are a per-unit resource bounded by the unit's `num_domains` (65536 on both targets); a simple incrementing allocator with an overflow assert is sufficient and honest at this stage.

Write the context entry with second-level translation type, the page table's physical root, and an address width chosen from the unit's SAGAW — **not** from the host's paging depth, since QEMU supports three levels and the R740 four. Flush the entry, then invalidate the context cache and IOTLB **of the unit that owns this device**, via `dmar_unit_for_bdf`.

Refuse to bind a device that `dmar_rmrr_covers` reports, per the spec: Phase 1 does not honour reserved regions, and binding such a device would break it silently.

For the demonstration, bind one device that is known to do DMA and is not needed for console or boot. On QEMU q35 the SATA controller at `00:1f.2` is in the DMAR scope and is idle once Composite has booted from the CD.

- [ ] **Step 4: Run to verify it passes**

```bash
./cos build && ./cos compose composition_scripts/unit_heap.toml baseline
timeout 40 ./cos run baseline iommu 2>&1 | tee /tmp/boot.log
grep -E "DMAR: bound|DMAR: fault" /tmp/boot.log
```

Expected: the bind line, and a recorded fault naming that device when it next attempts DMA.

**If no fault appears, do not record the task as passing.** A silent absence is exactly what a device that never issued a DMA looks like, and it is also what a broken context entry that left the device in pass-through looks like. Distinguish them: read back the context entry and confirm the translation type, and confirm the device is one that actually issues DMA in this configuration. An unfalsifiable pass is worse than a failure.

- [ ] **Step 5: Commit**

```bash
git add src/platform/x86_64/dmar.c src/platform/x86_64/dmar.h \
        src/kernel/include/shared/cos_types.h src/kernel/capinv.c
git commit -m "x86_64/dmar: device-to-domain binding and the containment demonstration"
```

---

## What this plan leaves for the next one

K7, the mirroring hook, and K9, the quiescence extension — the two hard ones, both now designable against a working bind and measured hardware. Then the userlevel work: `dmarmgr`, boot-time device assignment, and `nicmgr`'s switch to `--iova-mode=va`, which is where the containment property stops being a demonstration and starts protecting something real.
