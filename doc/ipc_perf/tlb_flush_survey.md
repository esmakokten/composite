# When must Composite flush the TLB? (x86-64)

**Date:** 2026-09-14
**Tree:** branch `asid_fix` (`efdef3a0` + `4e069fed`) on `main` @ `3ef8f8c4`
**Scope:** x86-64, PCID on (CR4.PCIDE=1), MPK off, no global pages. VT-x/EPT
(`INVEPT`/`INVVPID`) and MPK/ULK paths are excluded, as in CLAUDE.md.
**Sources:** Intel SDM Vol. 3A §4.10 *Caching Translation Information* (cited from
knowledge — section numbers and semantics are standard, but re-check exact wording
before quoting externally); Linux `arch/x86/mm/tlb.c` design, for comparison; and an
audit of the Composite source. Source line numbers are given only for files the
measurement scaffolding has not touched; elsewhere, function names.

---

## TL;DR

**The rule.** Invalidate whenever a translation the CPU may have cached becomes *less
permissive or points somewhere else*:
- a present leaf becomes not-present (unmap);
- its frame changes (remap);
- its permissions drop (R/W or U/S cleared, XD set);
- an upper-level entry is removed or replaced (subtree unlink).

Do it on **every CPU that may have cached it**, and before the frame — or the
page-table page — is reused.

**Never needed:**
- new mappings (not-present → present);
- permission increases (at worst a spurious fault);
- software-only PTE bits — Composite's `COSFRAME`/`COSKMEM`/`QUIESCENCE` are bits 9–11;
- entries that were never hardware-present — Composite's untyped `COSFRAME` frames.

**Composite x86-64 today invalidates in exactly one place:** the `mov %cr3` in
`chal_pgtbl_update()`, which flushes a PCID when it is loaded with a different page
table than last time on that core. Everything else is missing:
- `chal_flush_tlb()`, `chal_flush_tlb_global()` and `chal_remote_tlb_flush()` are empty;
- no INVLPG or INVPCID is used anywhere;
- the quiescence clocks that were meant to stand in for flushes are never advanced.

**Audit verdict** (§3.2): of the twelve page-table-changing operations,
- **seven are safe as they stand** — every "add", aliasing, frame delegation,
  kernel-object bits, and retype from untyped;
- **one is unreachable** (`mapping_mod`);
- **four gaps remain**, all in *removal*:
  - **revocation by unmap is not enforced** (known from PR #503);
  - **`DECONS` unlinks a page-table subtree with no invalidation and no quiescence** —
    the authors' own `FIXME` says so;
  - **lower-level page-table pages can be freed after ~1 ms of wall time, then retyped
    to user memory with no TLB check**, while a CPU may still hold a cached
    upper-level entry pointing at that page;
  - **an address-space switch cannot see content changes** to a page table it has
    cached.

All four are **latent**: no in-tree code unmaps, unlinks, or frees page-table memory at
runtime.

**Cost of fixing:** none of this touches the steady-state IPC fast path measured in
`ipc_path.md`. Invalidation cost lands only on unmap, unlink and free operations,
plus — if the periodic-flush design is restored — one flush per core per timer tick.

---

## 1. The x86 rules

### 1.1 What the CPU caches (§4.10.2, §4.10.3)

- **TLB entries** — leaf translations, created only when every entry on the walk is
  present. Tagged with the PCID; entries for global pages (G=1, with CR4.PGE) are
  shared across PCIDs and survive CR3 loads.
- **Paging-structure caches** — PML4E, PDPTE and PDE caches, so a TLB miss can skip the
  upper levels of a walk. Also PCID-tagged, and **the CPU may create one even when no
  leaf translation exists beneath that entry.** This is what makes unlinking a
  page-table page dangerous: a cached upper-level entry can keep pointing at a table
  page after the software has detached it.
- Both are **per logical processor**.

### 1.2 What invalidates what (§4.10.4.1)

| operation | TLB entries invalidated | paging-structure caches |
|---|---|---|
| `mov %cr3`, bit 63 = 0 | non-global entries of the PCID being loaded | that PCID's |
| `mov %cr3`, bit 63 = 1 (`CR3_NO_FLUSH`) | none | none |
| `INVLPG addr` | that page, **current PCID only** (plus global entries for it) | the current PCID's |
| `INVPCID` type 0 (pcid, addr) | that page in *that* PCID | that PCID's |
| `INVPCID` type 1 (pcid) | all non-global entries of that PCID | that PCID's |
| `INVPCID` type 2 | everything, all PCIDs, including global | all |
| `INVPCID` type 3 | all non-global, all PCIDs | all |
| `mov %cr4` toggling PGE (or changing PCIDE, PAE, PSE, SMEP) | everything, including global | all |

Practical consequence under PCIDs: **INVLPG cannot target a component that is not
running.** To invalidate in a page table that is not currently loaded, use INVPCID
type 0 or 1, or make that PCID's next `mov %cr3` flush. INVPCID is present on this host
and on the A03.

### 1.3 Must, may skip, may delay (§4.10.4.2–4.10.4.4, §4.10.5)

**Must invalidate** before relying on the change:
- a present leaf → not present (unmap);
- a leaf's physical frame changes (remap);
- R/W 1→0, U/S 1→0, XD 0→1 (downgrade);
- a page-size bit changes, or an upper-level entry is cleared or replaced. Here the whole
  range it covered is affected, including cached upper-level entries — in practice,
  flush the PCID;
- and **never reuse the old frame or page-table page until every CPU that may have
  cached it has invalidated.**

**May skip:**
- not-present → present — a new mapping, or filling an empty upper-level slot;
- permission increases (R/W 0→1, U/S 0→1, XD 1→0) — the worst case is a spurious fault
  the handler re-checks;
- clearing Accessed/Dirty — only affects whether the CPU sets them again, which matters
  to page-reclaim heuristics, not to isolation;
- software-available bits.

**May delay** (§4.10.4.4) if stale entries can do no harm. The standard technique: do
not reuse the old frame or table page until every CPU has flushed since the change.
Composite's quiescence design is exactly this (§2).

**Multiprocessor** (§4.10.5): CPUs currently running in the address space need an
immediate shootdown (IPI). CPUs not currently using it may defer to their next CR3 load
— *provided that load flushes*.

---

## 2. How others do it

- **Linux x86-64.**
  - Each `mm` carries a generation counter, `context.tlb_gen`, bumped on page-table
    changes.
  - Each CPU keeps a few dynamic PCID slots (`cpu_tlbstate.ctxs[]`, 6) recording
    `(ctx_id, tlb_gen)` — the same role as Composite's `tlb_asid_map`.
  - A change bumps `tlb_gen` and IPIs the CPUs in the mm's cpumask. A CPU switching to
    the mm flushes (CR3 without NOFLUSH) if its cached generation is stale.
  - Small ranges use per-page INVLPG, up to a ceiling of 33 pages; larger ranges flush
    the PCID.
  - Page-table pages are freed only after the flush (`mmu_gather`, `tlb_remove_table`)
    — precisely the property Composite's `DECONS` path lacks.
- **seL4.** Invalidation is synchronous with the unmap of a page or page table,
  shooting down cores that may have run the vspace, so an object is never reused before
  it is invalidated.
- **Composite's own design** — the Linux-hosted port, now archived:
  - every core flushes periodically from the timer and stamps `last_periodic_flush`;
    `tlb_mandatory_flush()` stamps `last_mandatory_flush`;
  - reuse of unmapped memory waits until every core has flushed since the unmap
    (`chal_tlb_quiescence_check()`) — an RCU-style grace period for TLBs;
  - it needs no per-unmap shootdown, but assumes the clocks advance. **Only that port
    ever advanced them** (`src/platform/archived/linux/module/hijack.c:2059–2063`, using
    Linux's own flush). x86-64 and armv7a have empty flush hooks and nothing that writes
    either timestamp.

---

## 3. Composite x86-64 audit

### 3.1 The invalidation machinery that exists

- **The only invalidating instruction** is the `mov %cr3` in `chal_pgtbl_update()`
  (`chal/chal_proto.h`). It flushes when the per-CPU `tlb_asid_map` says this PCID last
  held a different page table (correct since `efdef3a0`). It is reached from:
  - `sinv_call` and `sret_ret` (`inv.h`);
  - the thread switch in `capinv.c`, skipped when the page table is unchanged;
  - boot (`boot_comp.c:72`).
- **Empty hooks:** `chal_flush_tlb()`, `chal_flush_tlb_global()`,
  `chal_remote_tlb_flush()` (`chal/chal_plat.h`, on x86 and armv7a alike).
- **Dead code:** `tlb_mandatory_flush()` (`capinv.c`) is never called, and
  `last_periodic_flush` is never written. `chal_tlb_quiescence_check()`
  (`chal_pgtbl.c:150`) therefore grants quiescence only for a timestamp of 0.
- **No INVLPG/INVPCID** — `chal_cpu.h:215–218`: "Composite does not use INVLPG feature."
- **PTE layout** (`chal_pgtbl.h`):
  - hardware P/RW/US/PWT/PCD/A/D/PS/G are bits 0–8, PKEY bits 59–62, XD bit 63;
  - software `COSFRAME` = 9, `COSKMEM` = 10, `QUIESCENCE` = 11;
  - untyped frames are installed with `X86_PGTBL_COSFRAME` alone — **not present**
    (`boot_comp.c:180`) — so they are never in any TLB.

### 3.2 Every operation that changes page-table entries

| # | operation → function | entry change | invalidation needed? | what the code does | verdict | used at runtime in-tree? |
|---|---|---|---|---|---|---|
| 1 | `MEMACTIVATE` → `chal_pgtbl_mapping_add` (`chal_pgtbl.c:350`) | leaf: not present → present | No (0→1). Only if the slot holds a `QUIESCENCE` tombstone could an old translation still be cached — so reuse of that *vaddr* is gated | `pgtbl_quie_check()` (`:382`) — fails closed on x86-64 | **Safe.** Side effect: on x86-64 an unmapped vaddr can never be mapped again | yes — heap and page allocation |
| 2 | `CPY` (alias) → `chal_pgtbl_cpy` (`:708`) → `pgtbl_mapping_add` | destination leaf 0 → present | No | as #1 | **Safe** | boot, component setup |
| 3 | `MEMMOVE` → `cap_move` (`capinv.c`) | source `COSFRAME` entry → 0; destination → `COSFRAME` | No — `COSFRAME` entries are never hardware-present | destination gated by `pgtbl_quie_check()` | **Safe** | boot memory delegation (`__cos_meminfo_populate`) |
| 4 | kernel-object activate/deactivate → `chal_pgtbl_kmem_act` (`:88`), `kmem_deact_post` (`capinv.c`) | `COSKMEM` bit on a non-present `COSFRAME` entry | No — software bit, non-present entry | — | **Safe** | yes — object creation |
| 5 | `CONS` → `chal_pgtbl_cons` (`:747`) | upper-level entry: not present → present (refuses a present slot) | No on its own. Yes if the slot's previous table was unlinked without invalidation (#7) | — | **Safe on its own** | yes — table expansion, ULK map-in |
| 6 | `MEMDEACTIVATE` → `chal_pgtbl_mapping_del` (`:462`) | leaf present → `QUIESCENCE` tombstone | **Yes** — on every CPU that may hold it: before the frame is recycled, and before the unmap is relied on as revocation | stamps a liveness timestamp (`:495`); frame reuse gated by `last_unmap` (`retype_tbl.c:329`) → fails closed; **no invalidation** | **Recycling: safe but never reclaimable. Revocation: gap** — widened on the local core by the ASID fix, never enforced remotely | none (`cos_mem_remove` is `assert(0)`) |
| 7 | `DECONS` → `chal_pgtbl_decons` (`:794`) | upper-level entry present → 0; **no check that the subtree is empty** | **Yes** — flush the PCID(s) that used this table, on every CPU, before the table page is freed | **nothing.** `chal_pgtbl.c:740`: *"FIXME: we need to ensure TLB quiescence for pgtbl cons/decons!"* | **Gap** | none |
| 8 | `PGTBLDEACTIVATE(_ROOT)` → `chal_pgtbl_deact_pre` (`:850`) → `kmem_deact_post` | frees a page-table page | **Yes** — only after any cache pointing at it is invalidated | top level (lvl 0): `tlb_quiescence_check(frozen_ts)` → fails closed ✓. **Lower levels: time only** — `QUIESCENCE_CHECK(now, frozen_ts, KERN_QUIESCENCE_CYCLES)`, 2 M cycles ≈ 1 ms here, plus a page scan. The comment says the scan "will ensure tlb quiescence implicitly": true for leaf translations, **false for paging-structure caches** | **Gap** for lower-level page-table pages | none |
| 9 | `MEM_RETYPE2FRAME` → `retypetbl_retype2frame` (`retype_tbl.c:279`) | typed memory → untyped | **Yes**, if a cached translation could still reach it | `tlb_quiescence_check(max last_unmap)`. `last_unmap` is stamped only by the user unmap path (`retypetbl_deref`, `:140`), **never for kernel-typed memory** (`retypetbl_kern_deref`) → a freed page-table page presents 0 and passes | **Gap** — completes #7–#8 | none |
| 10 | `MEM_RETYPE2USER` / `2KERN` → `mod_mem_type` (`:170`) | untyped → typed; no entries change | No | — | **Safe** | yes — bump allocation |
| 11 | `chal_pgtbl_mapping_mod` (`:429`) | flags on a present leaf | Yes, if permissions drop | no invalidation | **Unreachable** — no capability op calls it | — |
| 12 | address-space switch — `chal_pgtbl_update` | loads a PCID | Flush if that PCID last held a different table, **or if the table's contents changed since** | handles the first; not the second (Copilot, PR #503) | **Gap** for content changes — same root cause as #6–#8 | every crossing |

### 3.3 The page-table-page gap, step by step

1. A component holding the capabilities `DECONS`es a lower-level page-table page out of
   its parent. In practice that is a memory manager: it needs the page-table and
   memory capabilities. The parent entry becomes 0, **with no invalidation**. Any CPU
   that walked through that entry may keep a PDE/PDPTE-cache entry pointing at the
   detached page, under that component's PCID.
2. After ~1 ms, `PGTBLDEACTIVATE` frees the page (#8). If the page's leaves were never
   mapped, the scan passes and nothing looks at TLB state.
3. `RETYPE2FRAME` (#9) sees `last_unmap == 0`, because kernel memory never stamps it,
   and makes the page untyped. `RETYPE2USER` plus `MEMACTIVATE` then hand it to a
   component as ordinary writable memory.
4. A later TLB miss in the original virtual range, on a CPU still holding the stale
   cache entry, may use it and **walk attacker-controlled bytes as a page table** —
   translations to arbitrary physical frames, kernel included.

Why it has not bitten:
- **nothing in-tree performs steps 1–3 at runtime**;
- before `efdef3a0`, every crossing into a component flushed that component's PCID,
  *including its paging-structure caches*. The ASID bug was quietly masking this, as it
  masked revocation (#6). The fix did not create the gap, but removed that incidental
  flush.

If `DECONS` detaches a table page whose leaves are still present, the page can never be
freed: the scan refuses, and `MEMDEACTIVATE` only works on top-level tables, so the
detached leaves are unreachable. That is a **leak, not a hole**.

### 3.4 Correction to `asid_fix.md`, and to the commit message

Two statements in `asid_fix.md`'s safety section were over-broad:
- that releasing a page-table kmem page "fails closed" via `frozen_ts` — true only for
  **top-level** tables (and for captbls, which the MMU never references);
- that a zero `last_unmap` "means never mapped, where granting quiescence is correct" —
  wrong for **page-table pages**, which are never user-mapped but can be cached as
  upper-level entries.

Both are corrected there. The pushed commit message for `efdef3a0` carries the first
over-broad sentence ("the quiescence checks gating retype, remap and kmem release fail
closed"). It is not rewritten here — that would need a force-push.

---

## 4. What Composite x86-64 would need

Ranked. None affects the steady-state IPC path; costs are qualitative — no flush
primitive was measured.

| # | change | fixes | cost profile |
|---|---|---|---|
| 1 | **Real primitives in `chal_plat.h`**: `chal_flush_tlb()` = INVPCID type 3 (or toggle CR4.PGE — equivalent while no page is global); `chal_flush_tlb_global()` = INVPCID type 2; new `chal_flush_pcid(asid)` (type 1) and `chal_invlpg_pcid(asid, va)` (type 0); `chal_remote_tlb_flush(cpu)` via a LAPIC IPI vector. | Prerequisite for everything below. | Only when called. |
| 2 | **Restore the periodic-flush design:** flush in the LAPIC timer handler on every core, stamping `last_periodic_flush` from a TSC read taken *before* the flush (the order `tlb_mandatory_flush()` already uses). | Makes `tlb_quiescence_check()` true to its name. Un-sticks recycling (#1, #6, #9, top-level #8), which today fails closed forever. **Caveat:** it also un-sticks freeing a component's *top-level capability table*, which a thread still running inside a deactivated component keeps using — close that first (`cap_dynamics_survey.md` §3.2). | One PCID flush per core per tick (100 Hz), with its refill charged to whatever runs next. |
| 3 | **Close the page-table-page gap:** require `tlb_quiescence_check()` for *every* level in `chal_pgtbl_deact_pre()` instead of the time-only check, stamping a timestamp at `DECONS`. Or, eagerly: flush the owning PCID(s) at `DECONS` and shoot down. | #7, #8, #9 — and the authors' `FIXME`. | Only on unlink and free. |
| 4 | **Revocation** (`MEMDEACTIVATE`, and `mapping_mod` if it is ever wired): eager INVPCID type 0 for the owning PCID, plus IPIs to CPUs running that address space. **Or** a per-page-table generation bumped on unmap and checked in `chal_pgtbl_update()` (Linux's `tlb_gen` model), with IPIs only to CPUs that currently have that table loaded. That requires tracking which CPUs have which table loaded — a per-table cpumask, as Linux's `mm_cpumask` or seL4's per-vspace bitmap. | #6, #12 — and the Copilot comment on PR #503. | One load and compare per crossing for the generation check; IPIs only on unmap. |
| 5 | A per-page vs whole-PCID flush threshold (Linux: 33 pages). | — | Only once batch unmaps exist. |

**Stays free:** new mappings, aliasing, frame delegation (`MEMMOVE`), kernel-object bits,
retype from untyped, and steady-state crossings.

---

## Not verified

- The SDM is cited from knowledge. Re-check the exact wording before external use —
  especially INVLPG and global pages, which Composite does not use today.
- **No flush primitive costs measured** — INVLPG, the INVPCID types, the CR4.PGE toggle,
  an IPI shootdown. A micro-op in the style of `k1f` (`ipc_path.md`) would measure them
  in one boot.
- The page-table-page gap (§3.3) is established by **code reading, not
  demonstration**. Demonstrating it needs a capability-holding component to run
  `DECONS` → deactivate → retype → remap and observe a stale upper-level-cache walk;
  whether a given CPU retains such a cache entry is SDM-permitted, not guaranteed.
- `KERN_QUIESCENCE_CYCLES` assumes 4000 cycles per µs; on this 2.10 GHz part its 2 M
  cycles are ~0.95 ms, not the intended 500 µs. Moot for TLB purposes, since time alone
  guarantees no flush here.
- MPK/ULK and EPT paths are out of scope.
