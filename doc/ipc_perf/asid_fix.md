# ASID double-extraction fix (`chal_pgtbl_update`)

**Date:** 2026-09-11
**Branch:** `asid_fix` on `esmakokten/composite` (pushed), on `main` @ `3ef8f8c4` —
`efdef3a0` the fix; `4e069fed` `ps` submodule → gwsystems/ps master `ca50b3f`.
**The fix:** commit `efdef3a0` = [`asid_fix.patch`](asid_fix.patch) — one file,
`src/platform/i386/chal/chal_proto.h` (x86_64's `chal/chal_proto.h` symlinks to it).
**Compositions:** `ping_pong.toml` and `sched_ping_pong.toml`
**Config:** MPK off, PCID on, `CPU_GHZ` 2.10, `NUM_CPU` 8. All `W1_*` toggles **off**
for every timing run; `W1_CR3_INSTRUMENT` on only for the QEMU counter runs.

---

## Result (bare metal, A03)

Raw TSC cycles, `N = 64` samples × `ITER = 1024`, `min / median / max`.

| composition | invocation | as-shipped | **fixed** | saved |
|---|---|---:|---:|---:|
| `ping_pong` | `pong_call` | 1130 / 1130 / 1130 ¹ | **851 / 851 / 852** | **~276 (24 %)** |
| `ping_pong` | `pong_argsrets` | 1124 / 1128 / 1131 ¹ | 863 / 864 / 866 | ~262 (23 %) |
| `sched_ping_pong` | `pong_call` | 1152 / 1154 / 1159 | **851 / 851 / 861** | **301 (26 %)** |
| `sched_ping_pong` | `pong_argsrets` | 1120 / 1121 / 1127 | 858 / 859 / 864 | 262 (23 %) |

¹ BM3 from 2026-09-10 (`baseline.md`). Same tree and harness; the only difference
from today's fixed `ping_pong` build is the fix itself (the `ps` one-liner below does
not touch `ping_pong`). The `sched_ping_pong` pair is a same-day A/B from builds that
differ **only** in the two helper functions.

**851 cycles = 405 ns** at 2.10 GHz, down from ~537 ns. Both compositions converge on
exactly 851: synchronous invocation never touches the scheduler, so once the
spurious flushes are gone the extra components are invisible to this path.

QEMU/KVM, for reference only: fixed `pong_call` 850–852 vs as-shipped 1346–1399.

---

## The fix

```diff
 struct cpu_tlb_asid_map {
-	pgtbl_t mapped_pt[NUM_ASID_MAX];
+	pgtbl_t mapped_pt[NUM_ASID_MAX + 1];
 } CACHE_ALIGNED;

 static inline pgtbl_t
-chal_cached_pt_curr(prot_domain_t protdom)
+chal_cached_pt_curr(u16_t asid)
 {
-	u16_t asid = PROTDOM_ASID(protdom);
 	return tlb_asid_map[get_cpuid()].mapped_pt[asid];
 }

 static inline void
-chal_cached_pt_update(pgtbl_t pt, prot_domain_t protdom)
+chal_cached_pt_update(pgtbl_t pt, u16_t asid)
 {
-	u16_t asid                                = PROTDOM_ASID(protdom);
 	tlb_asid_map[get_cpuid()].mapped_pt[asid] = pt;
 }
```

(plus two explanatory comments — see the patch).

- **Why this shape.** Both helpers have exactly one call site — `chal_pgtbl_update`,
  which already holds the extracted `u16_t asid`. Making the helpers take an ASID
  removes the second `PROTDOM_ASID()` with no other callers to update, and the
  parameter type now says what the value is.
- **Array bound.** `NUM_ASID_MAX` is 4095, the *maximum value* `PROTDOM_ASID()` can
  return, so the array needs 4096 slots. The bug hid this (the index always collapsed
  small). With the fix, ASID 4095 — allocatable, `PROTDOM_ASID_NUM_NAMES` is 4096 —
  would have written one past the end. No layout change: `CACHE_ALIGNED` rounds both
  4095×8 and 4096×8 bytes to the same 32 KB per CPU.

## Verification

- **Counters** (`W1_CR3_INSTRUMENT`, QEMU, `sched_ping_pong`, benchmark core):

  | | calls | `noflush_hits` | `flush_misses` |
  |---|---:|---:|---:|
  | as-shipped | 266 312 | **0** | 266 312 |
  | fixed | 266 302 | **266 296** | **6** |

  The 6 remaining misses are one cold-start flush per ASID (`asid_seen_mask = 0x3f`,
  all six components). `noflush_hits` now **equals** `would_hit_if_correct` exactly:
  the real cache behaves precisely like the shadow correctly-indexed model from
  yesterday's diagnosis. Same picture on `ping_pong` (266 261 hits, 7 misses).
- **Correctness.** All `pong_*` asserts pass in both compositions, QEMU and bare metal.
  The scheduler's own test (`unit_schedcomp`) still passes: "Highest was scheduled
  only!", "We swapped back and forth!".
- **Disassembly** of the linked kernel: the fixed handler indexes
  `cpu*4096 + asid` (`shl $0xc`), sets bit 63 with `bts $0x3f` on the hit path, and
  has exactly two fewer `shr $0x4` than the as-shipped build — the removed second
  extractions. The as-shipped comparison ISO was checked the same way before use.
- **The committed branch, stock benchmark.** Built exactly as committed (measurement
  scaffolding parked) and booted three times under QEMU with `sched_ping_pong` and the
  unmodified `ping.c`: `Fast-path` 853 / 853 / 873, `Three return` ≈860 / 867 / 865
  (one-pass means, vs ~1350–1400 as-shipped). Without the console lock the output is
  interleaved character by character, so the numbers were de-interleaved from
  alternating streams; an interleave-tolerant search found no fault markers.
- **"Too good to be true" check** (CLAUDE.md). The work that should still happen does:
  `mov %cr3` still executes on every sinv and sret (2 per round trip in the handler),
  so the components are still in separate page tables. Only the flush bit changed, and
  it changes only where the cached-pgtbl check says the flush is redundant. The
  required flushes — cold start per ASID, and a different page table loaded into an
  already-used PCID (see the booter note) — still happen.

## Safety: why skipping these flushes is sound

The fast path now skips a TLB flush on nearly every crossing. That is only correct if
nothing depended on those flushes, so this was audited *before* the change.

1. **What the cache guarantees.** `NO_FLUSH` only when the page table being loaded
   under this ASID on this core is the one loaded there last time. A different page
   table in the same PCID flushes. The map is per-CPU, like the TLB. Entries start
   zeroed (BSS), so the first load of every ASID flushes — which also clears whatever
   the boot page table (loaded into PCID 0 by `chal_cpu_pgtbl_activate`) left behind.
2. **What it does not guarantee:** invalidation when a page table's *contents* change
   under the same root. Adding mappings needs none on x86 (non-present translations
   are not cached). Removing or downgrading one does.
3. **Did anything rely on the every-crossing flush for that? No enforced invariant.**
   - `chal_flush_tlb()`, `chal_flush_tlb_global()` and `chal_remote_tlb_flush()` are
     **empty** on x86_64/i386 (`chal/chal_plat.h`).
   - `tlb_mandatory_flush()`, the only caller of `chal_flush_tlb()`, is **never called**.
   - `last_periodic_flush` is **never written** on x86_64.
   - So `chal_tlb_quiescence_check()` compares every timestamp against zero and
     answers "not quiescent" for **any stamped timestamp** (`ts > 0`).

   Every path that *recycles* memory after a **leaf** unmap is gated by a stamped timestamp,
   so each one fails closed with `-EQUIESCENCE`. The core audit (single cache user,
   dead flush machinery) was done before the change; the timestamp sources below
   were verified afterwards, while tightening this section:

   | recycling path | gate | timestamp stamped by |
   |---|---|---|
   | retype a frame back to untyped | `last_unmap` (`retype_tbl.c:329`) | `rdtscll` in `retypetbl_deref()` (`retype_tbl.c:140`), called by the unmap path |
   | map again at a vaddr that was unmapped | the `QUIESCENCE` PTE's liveness id (`chal_pgtbl_quie_check()`, `chal_pgtbl.c:645`; leaf scan `:545`) | `ltbl_timestamp_update()` → `rdtscll(deact_timestamp)`, called by the unmap path *before* it writes the PTE (`chal_pgtbl.c:495`) |
   | release a **top-level** page table (or any captbl, which the MMU never references) | `frozen_ts` (`kmem_deact_pre()` → `chal_pgtbl_deact_pre()`, `chal_pgtbl.c:867`) | the freeze op: `rdtscll(frozen_ts)` *before* setting `FROZEN` (`cap_ops.h:176/184`) |
   | release a **lower-level** page-table page | ***time only*** — `QUIESCENCE_CHECK(now, frozen_ts, KERN_QUIESCENCE_CYCLES)` ≈ 1 ms, plus a page scan (`chal_pgtbl.c:850`) | **none — see the correction below** |

   Two `FROZEN` writes carry no stamp (`captbl.c:92`, `chal_pgtbl.c:297`), but both
   are the last step of a deactivation that has already passed `kmem_deact_pre()`
   (called at `captbl.c:81` and `chal_pgtbl.c:289`), or are the non-root case, which
   frees nothing. A zero timestamp reaches the retype check only for a set never
   unmapped since init with no live mappings — i.e. never mapped — where granting
   quiescence is correct.

   > **Correction (2026-09-14, from [`tlb_flush_survey.md`](tlb_flush_survey.md) §3.3).**
   > Two statements here were too broad. The `frozen_ts` gate covers only **top-level**
   > page tables: a lower-level page-table page is released after ~1 ms of wall time
   > plus a scan, with no TLB check. And a zero `last_unmap` does not mean "never
   > mapped" for such a page: page-table pages are kernel-typed, so nothing ever stamps
   > their `last_unmap`, yet a CPU can still hold a cached upper-level entry pointing
   > at one after `DECONS` unlinks it (with no invalidation). A freed page-table page
   > can therefore be retyped to user memory while a stale paging-structure-cache
   > entry still walks through it. Latent — nothing in-tree unlinks or frees page-table
   > pages at runtime — and masked before the fix by the every-crossing PCID flush.
   > The pushed `efdef3a0` commit message repeats the over-broad claim.

   So a stale *leaf* translation cannot expose recycled memory: that is blocked upstream,
   and the check gives identical answers before and after the fix (it reads only
   timestamps). For recycling, the buggy flush was incidental, not load-bearing.
4. **Residual exposure — pre-existing, but now more reachable: revocation by unmap.**
   Quiescence gates recycling, not revocation. Take a USER frame unmapped from
   component A — to take A's access away — then aliased into component B at a fresh
   vaddr, without a retype. That hits no quiescence check I found: the map-side check
   is keyed on the destination vaddr's PTE, and the frame's own `last_unmap` is
   consulted only on retype to untyped. (The frame-refcount path, `retypetbl_ref`,
   was not read.) A's PCID may still hold the stale translation. Before the fix, the
   next crossing into A flushed A's PCID on that core; now it does not, so A can keep
   using the frame there until its PCID misses for another reason. On other cores it
   was never enforced (remote flush is empty). Anything relying on unmap for
   revocation needs a real invalidation at the unmap site (`invlpg`/`invpcid`, or a
   working `chal_flush_tlb()` that is actually called). These compositions do not
   revoke at runtime; **other components are not audited.**

## Also found

- **The booter shares PCID 0.** `crt_booter_create()` never takes a protdom — llbooter
  allocates one for it and discards it — so the booter runs under ASID 0, shared with
  whichever component is allocated ASID 0. The counters show it: in `ping_pong` the
  non-benchmark cores see only ASID 0, loaded with three different page tables, and
  miss each time. With the fix those flushes are correct and required; the cost is
  init-only. Worth fixing in the allocator eventually (give the booter its ASID, or
  reserve 0 for it).
- **i386** shares this header. There `NUM_ASID_MAX` is 0, so the array goes from a
  zero-length array indexed at 0 (undefined behaviour) to one slot, and protdom is
  always 0. Reasoned, **not built**.

## Where the 851 cycles go now

> **Superseded 2026-09-14 by [`ipc_path.md`](ipc_path.md).** The table below came
> from the perturbed `rdtscp` probe plus a residual, and two buckets were badly
> wrong: the probe under-measured `mov %cr3` (~107 per crossing; really ~256), and
> the leftover was dumped on the user-level stubs (~147; really ~32). An additive
> measurement — per-mechanism capabilities plus `rdpmc`, no probes on the real
> path — decomposes the round trip with **no residual**:
>
> | bucket | cycles | share |
> |---|---:|---:|
> | 2 × `mov %cr3` (`NO_FLUSH`) + ASID bookkeeping | **512** | 60 % |
> | 2 × bare kernel crossing | **~260** | 30 % |
> | invocation-stack copy (packed → byte loop) | 31 | 3.6 % |
> | capability checks (lookup + 2 × liveness) | 13 | 1.5 % |
> | other kernel bookkeeping | ~5 | 0.6 % |
> | server stub | 24 | 2.8 % |
> | client trampoline + C stub | 8 | 0.9 % |
> | **total** | **853** | |
>
> Per round trip there are also **0** branch mispredicts, **0** i-cache misses and
> **0** page walks. The superseded table is kept below for the record.

| bucket (fixed round trip) | cycles | confidence |
|---|---:|---|
| kernel entry + exit, both crossings | 260 | hard — null cap, QEMU and metal agree |
| `mov %cr3` with `NO_FLUSH` + pgtbl bookkeeping, ×2 | ~214 | **wrong, ~512** — the probe under-measured it |
| capability dispatch, ×2 | ~50 | soft (probe) |
| `sinv_call` C body (incl. the packed 32-byte byte-loop copy, ~40–80) | ~115 | soft (probe) |
| `sret_ret` C body | ~65 | soft (probe) |
| user side: trampolines, stub stack switch, pong's body | ~147 | **wrong, ~32** — this was a residual |
| **total** | **851** | = measured |

## Correction to `baseline.md`

`baseline.md` projected the fix at "~745 cyc, a 385-cycle / 34 % recovery". That was
the instrumented `FORCE_NOFLUSH` A/B (1264 → 879) minus an assumed-constant 134-cycle
instrument overhead. The assumption is wrong: the counters' own loads and stores sit
inside the CR3 path and are slower when every crossing flushes, so the overhead was
larger in the flushing build and the delta did not cancel. The real fix lands at 851.
`baseline.md` has been corrected in place, with each change marked.

## `sched_ping_pong` notes

- **Build fix required.** `capmgr.simple` needs `PS_LOCK_INITIALIZER`, which the `ps`
  submodule at `f7bcdbe` lacks. The measurements above used a local one-line copy of
  it; that is superseded by `4e069fed`, which moves the submodule to gwsystems/ps
  master (`ca50b3f`, same definition; the rest of that update is comment-only
  `ps_list.h` fixes and a standalone example). The local edit is kept in the
  submodule's stash.
- **Scheduler tick** (ignored per instruction, noted for completeness). It adds a
  small upper mode: fixed `pong_call` has 43 of 64 samples at 851 and a cluster at
  856–857, max 861, versus 62 of 64 at 851 without a scheduler. Min and median are
  unaffected.
- With a real scheduler, ping's `main()` runs to completion. That is consistent with
  yesterday's diagnosis that the lost-`main()` race is specific to the no-scheduler
  llbooter path.

## Raw data — per-rep totals/ITER, 64 samples (bare metal)

`sched_ping_pong`, as-shipped (`log_ipc-bug-sched.txt`)
```
pong_call     1152 ×18, 1153 ×13, 1154 ×30, 1157 1158 1159
pong_argsrets 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1120 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1121 1122 1122 1122 1122 1122 1122 1122 1122 1123 1123 1123 1124 1124 1124 1124 1124 1124 1124 1124 1124 1124 1124 1124 1125 1126 1126 1127
```

`sched_ping_pong`, fixed (`log_ipc-fix-sched.txt`)
```
pong_call     851 ×43, 856, 857 ×19, 861
pong_argsrets 858 858 858 858 858 858 858 858 858 858 858 858 858 858 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 859 860 860 860 860 860 860 860 860 860 860 860 860 861 861 861 861 861 861 861 861 861 861 861 863 864 864
```

`ping_pong`, fixed (`log_ipc-fix-pp.txt`)
```
pong_call     851 ×62, 852 ×2
pong_argsrets 863, 864 ×43, 865 ×19, 866
```

Full logs: `/home/syslab/cos-logs/log_ipc-{bug-sched,fix-sched,fix-pp}.txt`.

## What in the tree is the fix, and what is not

| change | status |
|---|---|
| `chal_proto.h` helper signatures + array bound | **the fix** — committed and pushed, `efdef3a0` |
| `ps` submodule → gwsystems/ps master `ca50b3f` (`PS_LOCK_INITIALIZER`) | committed and pushed, `4e069fed` |
| `crt.c` `crt_clear_schedevents` | separate real bug (no-scheduler `ping_pong` boot), from 2026-09-10 |
| `printk.c` console lock | readability; keep-worthy |
| `W1_*` instrumentation in `chal_proto.h`, `chal.c`, `capinv.c`, `inv.h`, `cos_config.h` | measurement only, all compiled out; **must not ship** — `W1_CR3_FORCE_NOFLUSH` especially |
| llbooter ASID `printc`, `ping.c` harness | measurement only |

## Not verified

- TLB-miss and page-walk counts (no PMU counters wired up): the saving is measured,
  the number of translations it preserves is not.
- Runtime unmap/downgrade users across all components (see *Safety* 4). Only these two
  compositions were exercised.
- The ~214-cycle `mov %cr3` bucket is derived from the perturbed probe, not measured
  directly.
- i386, and MPK-on / shared-VAS configurations.
