# Composite synchronous-IPC baseline and cycle attribution

**Date:** 2026-09-10
**Tree:** `esmakokten/composite` @ `3ef8f8c4d3296624640e6f3bd00801054d8350a3`
(main, "Merge pull request #502 from esmakokten/vm_image")
**Composition:** `composition_scripts/ping_pong.toml`
(`no_interface.llbooter` → `tests.unit_pingpong` (ping) → `pong.pingpong` (pong);
no capmgr, no scheduler)
**Vehicle:** `pong_call()` (register-only fast path, 0 args / 0 rets) and
`pong_argsrets()` (4 args + 2 rets, via the C stub).

---

## TL;DR

| | bare metal (A03) | QEMU/KVM |
|---|---:|---:|
| **`ping → pong → ping` round trip, `pong_call()`** | **1124–1130 cyc** (min≈median, spread 0–1) | 1346–1399 cyc (spread 20–80) |
| **round trip, `pong_argsrets()`** | **1124–1128 cyc** | 1360–1443 cyc |
| bare kernel crossing, one direction (null syscall) | 130 cyc | 130 cyc |

At 2.10 GHz the bare-metal round trip is **~537 ns**. `pong_call` and
`pong_argsrets` are equal to within 4 cycles — the extra marshalling is free at
this scale.

**The single biggest attributable cost is a bug.** A double application of
`PROTDOM_ASID` in `chal_pgtbl_update` means `CR3_NO_FLUSH` is *never* set: every
`sinv` and every `sret` does a PCID-flushing `mov %cr3`. This is H1, and it is
confirmed (evidence below).

> **Correction (2026-09-11) — see `asid_fix.md`.** This file originally projected
> the fix at "~745 cyc, a 385-cycle / 34 % recovery". That figure came from the
> *instrumented* `FORCE_NOFLUSH` A/B (1264 → 879) minus an assumed-constant
> 134-cycle instrument overhead. The overhead is not constant: the counters sit
> inside the CR3 path and cost more when every crossing flushes. The real fix,
> measured clean on bare metal, gives **1127 → 851 cyc: ~276 cycles / 24 %**
> (`ping_pong`) and **1152 → 851: 301 / 26 %** (`sched_ping_pong`). Figures below
> that depended on 385 / ~745 have been corrected in place and marked. A later
> additive decomposition of the *fixed* path is in [`ipc_path.md`](ipc_path.md); it
> supersedes the Phase-3 soft buckets below (`mov %cr3` is ~256 per crossing, and
> the user-level stubs ~32 in total, not ~147).

---

## Configuration in force (state these with every number)

| flag | value | where |
|---|---|---|
| MPK | **OFF** | `cos_config.h:98` `// #define MPK_ENABLED` → `chal_protdom_write/read` are no-ops, no `wrpkru`/`rdpkru` on the path |
| PCID | **ON** (CR4.PCIDE=1) | `chal_cpu_init`, `chal_cpu.h`; `invpcid`+`pcid` present in host `/proc/cpuinfo` |
| `CPU_GHZ` | **2.10** | generated `cpu_ghz.h` — non-empty, correct (`./cos init` regenerated it) |
| `NUM_CPU` | **8** | `cos_config.h:20` `NUM_CPU_KERNEL 8`; kernel asserts if the machine has ≠8 LAPICs → QEMU must run `-smp 8` |
| page global bit | **unused** | `PGTBL_GLOBAL` is defined but set by no mapping path → every CR3 flush loses *all* of a PCID's entries, kernel included |
| TSC | invariant | host + A03: `constant_tsc`, `nonstop_tsc` confirmed |
| host / A03 | 2× Xeon Platinum 8160 @ 2.10 GHz, 48c/2s (8 used) | Skylake-SP |

---

## Phase 1 — what had to change to build and boot

`ping_pong.toml` **does not run to completion on current `main`.** Three changes
were needed; only the first two are candidates to keep.

### 1. `crt_clear_schedevents()` assertion (real bug, off the IPC path)

`src/components/lib/crt/crt.c:1392`

```c
-	while (pending) {
+	while (pending > 0) {
 		pending = cos_sched_rcv(sched_aep->rcv, RCV_NON_BLOCKING, TCAP_TIME_NIL, ...);
-		assert(pending >= 0);
 	}
```

In the no-scheduler config, `cos_sched_rcv(RCV_NON_BLOCKING)` returns a negative
errno when nothing is pending. The shipped `assert(pending >= 0)` fires inside
`crt_compinit_execute` (run by llbooter on every core), does the user-level
assert's deliberate `*NULL = 0`, and page-faults the booter **before ping's
`main()` runs**. Root cause of the negative return not chased — it is boot
orchestration, not the invocation path.

### 2. `printk` console lock (readability only)

`src/platform/i386/printk.c` — a `pause`-spin test-and-set around the handler
loop. Without it, 8 cores writing the serial port char-by-char make every
multi-core fault/boot log unreadable. Safe to keep.

### 3. Benchmark rewrite — `src/components/implementation/tests/unit_pingpong/ping.c`

The shipped benchmark is one pass of 1024 iterations, mean only, printed from
`main()`. Replaced with:

- **warmup** — 1024 invocations of each type before timing (first-touch faults,
  cold i-cache on `__ipc_entry`, cold branch predictors all land in iter 1).
- **repetition** — `REPS = 64` samples of `ITER = 1024` invocations; report
  **min / median / max / spread** + the raw 64-sample list. Min ≈ true path
  cost; on bare metal min = median (see "spread" below).
- **instrument subtraction** — an empty loop with the same `rdtscp` brackets is
  timed; it costs < 1 cyc/iteration amortised (one `rdtscp` pair per 1024), so
  it is reported as 0 and subtracted as 0.
- **timestamp** — `rdtscp` (waits for prior instructions to retire) at the loop
  brackets, not inside. The loop body contains a fully-serialising `syscall`, so
  even a bare `rdtsc` could not reorder across 1024 of them; `rdtscp` is used
  anyway because it is strictly safer and free here.
- **prints from `cos_init()`**, not `main()` — the no-scheduler llbooter races
  ping's single-`main` against `cos_hw_shutdown` on another core, and the
  `main()` output is lost. `cos_init` completes reliably (verified: the
  "initialization complete" line always prints before the fault window).
- **pinning** — not explicit, but `cos_init` runs single-threaded on ping's init
  core with no scheduler to migrate it; TSC is invariant and per-core-consistent.

Spread on bare metal is **0–1 cycle**. That is not a measurement artefact.
*Corrected 2026-09-11:* this originally attributed the determinism to the bug
("every iteration runs equally cold"). The fixed build contradicts that: its spread
is also 0–1 (851 on 62 of 64 samples, `asid_fix.md`), so the path is deterministic
on this core with or without the flushes. Under KVM the spread is 20–80 cyc
(EPT-walk variability).

---

## Phase 2 — the numbers

All values are **raw TSC cycles**, `N = 64` samples × `ITER = 1024` invocations,
reported `min / median / max`.

### Bare metal (A03, via `composite-testrun`; serial capture live before boot)

| run | kernel build | `pong_call` | `pong_argsrets` | null-syscall (1 crossing) |
|---|---|---|---|---|
| **BM6** | **as-shipped** (+ cheap null cap) | **1124 / 1124 / 1125** | **1126 / 1126 / 1127** | **130** |
| BM3 | as-shipped, nothing added | 1130 / 1130 / 1130 | 1124 / 1128 / 1131 | — |
| BM1 | + `W1_CR3_INSTRUMENT` (H1 counters) | 1264 / 1264 / 1265 | 1262 / 1262 / 1273 | — |
| BM2 | + `W1_CR3_FORCE_NOFLUSH` + instrument | 879 / 879 / 880 | 882 / 887 / 889 | — |
| BM4 | + `W1_PROBE` (rdtscp probes, perturbed) | 1435 / 1436 / 1436 | 1434 / 1434 / 1436 | 205 |
| BM5b | + `W1_CR3_FORCE_NOFLUSH` alone | **page-faults in pong `cos_init`** | — | — |

*Take-away numbers:* **as-shipped round trip = 1124–1130 cyc** (BM3/BM6 agree);
`W1_CR3_INSTRUMENT` adds ~134 cyc of counter bookkeeping on the CR3 path *in this
flushing build* (BM1 − BM3); `W1_PROBE` adds ~305 cyc of `rdtscp` serialisation (BM4).
*Corrected 2026-09-11: this originally said "a flat ~134". The overhead is not
constant across builds — it is smaller when crossings don't flush — and that is what
invalidated the "~745 if fixed" projection (see the correction at the top).*

BM5b is important: `FORCE_NOFLUSH` **without** the instrument counters faults
during pong's initialisation (NULL/`RSP=0` register state → a botched stack
switch). It is an *unsafe* measurement hack — turning off flushing entirely
breaks the mapping changes that happen during init. BM2 (same hack, *with* the
instrument counters, which perturb the page-touch timing) happened to survive
init and its steady-state loop measurement is valid.

### QEMU / KVM (same host, `-enable-kvm -cpu max -smp 8 -m 8192`, `pong_call` min)

| build | min | spread | notes |
|---|---:|---:|---|
| as-shipped | 1346–1399 | 20–80 | ±10 % run-to-run; core-assignment varies |
| + `W1_CR3_INSTRUMENT` | 1378–1510 | 26–42 | |
| + `W1_CR3_FORCE_NOFLUSH` | 879 | 20 | |
| + `W1_PROBE` | 1890 | 34 | |
| null-syscall (clean) | 130 | — | **identical to bare metal** |

**QEMU vs bare metal gap: ~ +20 % on the round trip (≈1370 vs ≈1127), and 20–80×
worse spread.** The path is dominated by CR3 writes and the TLB refills that
follow them (which the bug triggers on every crossing); under KVM each of those
refills also walks the EPT. The null-syscall number, which touches no page-table
state, is bit-identical across the two — a good check that the harness itself is
sound.

---

## Phase 3 — cycle attribution (bare metal, `pong_call` round trip = 1127 cyc)

Two methods. The **subtractive** one (rebuild with a mechanism neutralised, diff
the totals) is trusted; the **probe** one (`rdtscp` inside the kernel path) is a
cross-check and is openly perturbed.

### High confidence — independent clean measurements

| bucket | cycles | how |
|---|---:|---|
| kernel entry + exit, **both** crossings (`swapgs`×2, `SAVE/RESTORE_REGS_ALL` = 15 push/pop + `sub/add 0x50`, `call`/`ret`, `sysretq`, `syscall`) | **260** | null cap (cap `0xf00` returns before any dispatch); 130/crossing; QEMU and bare metal agree exactly |
| **CR3 flush + TLB/PSC refill forced by the ASID bug**, both crossings | **~276** | clean A/B with the actual fix on bare metal: 1127 → 851 (`asid_fix.md`). *Corrected: originally 385, from the instrumented `FORCE_NOFLUSH` A/B.* |
| **subtotal (hard)** | **~536** | **48 % of 1127** *(corrected from 645 / 57 %)* |

### Probe-attributed — de-perturbed estimates, ±~30 % each (BM4)

BM4 adds ~9 `rdtscp` per round trip (~305 cyc total inflation; each bucket
carries ~2 `rdtscp` ≈ 68 cyc that is subtracted out below).

| bucket | raw probe | est. true | content |
|---|---:|---:|---|
| `mov %cr3` itself + `protdom_write` + `PROTDOM_ASID` + cache-check branch, ×2 | pgtbl 294+296 | **~214** | pgtbl bucket (~490 real) minus the ~276 flush *(corrected from ~105)* |
| capability dispatch, ×2 | `sinv_pre` 55 | **~50** | `thd_invstk_current`, `captbl_lkup`, `ch->type` check |
| `sinv_call` C body | 480−55−294 | **~115** | `ltbl_isalive`, `thd_invstk_push` (incl. the comp_info copy — see H2), `__userregs_setinv` |
| `sret_ret` C body | 373−296 | **~65** | `thd_invstk_pop`, `ltbl_isalive`, `__userregs_set` |
| **subtotal (soft)** | | **~444** | *(corrected from ~335)* |

### Residual

```
measured (1127) − hard (~536) − soft (~444) = ~147 cyc   (corrected split; residual unchanged)
```

The residual is the **user-side half of the round trip**: the client trampoline
(`call *INVFN(%rax)` + the C stub), the server `cos_asm_stub` stack switch
(`COS_ASM_GET_STACK_INVTOKEN`, `and $~0xf,%rsp`, token load), pong's (trivial)
function body, and the `RET_CAP` syscall arming on the way back. ~147 cyc for
two stack switches and two trampolines is reasonable.

**Honest summary: ~536 cyc attributed hard, ~591 cyc attributed soft (probe +
residual), ~0 genuinely unexplained.** *(Corrected from 645 / ~482.)* The soft buckets sum correctly against
the total but individually rest on the perturbed probe.

### What the two methods say about the CR3 path, reconciled

- Subtractive (clean A/B with the real fix): the **flush** costs ~276/round trip
  (~138/crossing). *Corrected from 385.*
- Probe: the **whole `pgtbl_update` region** costs ~490/round trip (~245/crossing).
- Difference ~214 (~107/crossing) = the `mov %cr3` instruction itself (serialising
  even with `NO_FLUSH`), plus `PROTDOM_ASID`, the `tlb_asid_map` load, and the branch.
- Consistent. The fixed-path breakdown (851 cyc) is in `asid_fix.md`.

---

## Phase 4 — the two hypotheses

### H1 — ASID double-extraction → TLB flush on every crossing — **CONFIRMED**

**The code.** `chal_pgtbl_update` (`chal_proto.h:171`):

```c
u16_t asid = PROTDOM_ASID(pt->protdom);              // correct: (protdom >> 4) & 0xFFF
...
if (likely(chal_cached_pt_curr(asid) == pt->pgtbl))  // <-- passes the ALREADY-extracted asid
        cr3 |= CR3_NO_FLUSH;
else
        chal_cached_pt_update(pt->pgtbl, asid);       // <-- same
```

`chal_cached_pt_curr` / `chal_cached_pt_update` both do
`u16_t asid = PROTDOM_ASID(protdom);` on the value handed to them — a **second**
`(x >> 4) & 0xFFF`. For any real asid < 16 the second extraction yields **0**.

**The ASIDs the composer/booter actually assign** (printed every boot by an
added `printc` in llbooter's `addrspc_exclusive` loop):

```
component 3 'tests.unit_pingpong' (ping)  protdom=0x00  asid=0   mpk_key=0
component 2 'pong.pingpong'        (pong)  protdom=0x10  asid=1   mpk_key=0
component 1 'no_interface.llbooter'        protdom=0x20  asid=2   mpk_key=0
```

`protdom_ns_asid_alloc` (`protdom.c:177`) just returns the first free name,
starting at 0. **ping=0, pong=1, both < 16** → both index
`tlb_asid_map[cpu].mapped_pt[0]`.

**Runtime counters** (`W1_CR3_INSTRUMENT`, one full benchmark run, benchmark
core, 266 268 `chal_pgtbl_update` calls = 133 134 round trips × 2):

```
calls                = 266268
noflush_hits         = 0          <-- CR3_NO_FLUSH is NEVER set. Not once.
flush_misses         = 266268     <-- every crossing takes the flushing path
would_hit_if_correct = 266265     <-- indexed by the REAL asid, 99.999% would have hit
real_asid_switches   = 266266     <-- ping/pong alternate asid every single call
asid_seen_mask       = 0x3        <-- asids 0 and 1
```

**Subtractive confirmation.** Force `cr3 |= CR3_NO_FLUSH` unconditionally:

| | as-shipped | forced no-flush | Δ / round trip |
|---|---:|---:|---:|
| instrumented (BM1 vs BM2) | 1264 | 879 | 385 |
| clean, *inferred* (BM3; 879 − 134) — **invalid, see correction** | 1130 | ≈745 | ≈385 |
| **clean, actual fix (2026-09-11, `asid_fix.md`)** | **1127** | **851** | **~276** |

**Mechanism.** With PCIDE=1, `mov cr3` without bit 63 invalidates all non-global
TLB + paging-structure-cache entries **for the PCID being loaded**. No Composite
page is global (`PGTBL_GLOBAL` unused), so each crossing throws away the entire
translation working set of the domain being *entered* — including the kernel's —
and the next instructions in pong (or ping) walk the tables from scratch, twice
per round trip. The bug converts a would-be zero-flush crossing into a
full-domain reload.

**Cost of the bug: ~276 cyc / ~24 % of the round trip** — measured with the actual
fix (1127 → 851 on bare metal). *Corrected: this line originally said ~385 / ~34 %
and "~745 if fixed", an artefact of the instrument-overhead assumption.*

**Not verified:** the split of the flush cost between the microcoded PCID-flush
operation and the subsequent page-walk refills — I showed *that* the flush
happens and *what it costs in total*, not how many entries are lost or the walk
count. That needs PMU counters (`dtlb_load_misses.walk_*`, `itlb_misses.walk_*`),
which were not wired up. *(The other open item here — whether a correct fix lands
at ~745 — is resolved: it was built and measured on 2026-09-11 at 851.)*

### H2 — the `comp_info` memcpy in `thd_invstk_push` — **small, but more interesting than expected**

`struct comp_info` = `liveness_data` (u64 + u32 = 12 B) + `pgtbl_info`
(ptr 8 B + `prot_domain_t` u32 4 B = 12 B) + `captbl *` (8 B) = **32 bytes**,
`__attribute__((packed))`. So the size guess in the brief is right: ~32 B, and
it is on the `sinv` path only (`sret`'s `thd_invstk_pop` returns a pointer, it
copies nothing).

But the generated code is **not** "a few stores" — from the linked kernel
(`composite_syscall_handler`, the inlined `thd_invstk_push`):

```asm
    xor    %eax,%eax
1:  movzbl 0x4(%rbp,%rax,1),%edx      ; load ONE byte  (sinvc->comp_info + i)
    mov    %dl,(%rsi,%rax,1)          ; store ONE byte (top->comp_info + i)
    add    $0x1,%rax
    cmp    $0x20,%rax                 ; 32
    jne    1b
```

A **32-iteration byte copy**, because `struct comp_info` (and `struct cap_sinv`)
are `packed` → the compiler cannot assume alignment → it will not emit `movq`s or
`movdqu`. Measured later at **40 cycles isolated, 31 end-to-end** (`ipc_path.md`),
not single digits.

**Verdict:** not a dominant cost (3.6 % of the round trip, measured; sinv side only), but
a real and *safe* optimisation — it is a kernel-internal copy of already-trusted
data. Ranked #2 below.

### Third cost centre bigger than H1 or H2? — **No.**

After the flush (~276; *corrected from 385*) and the irreducible entry/exit (260),
the largest remaining item is the no-flush `mov %cr3` itself — ~214 across both
crossings, probe-derived (*corrected from ~105*). It is serialising, and removing
it means not switching page tables at all, which changes the isolation model (see
`asid_fix.md`). The byte-copy (H2) is the only other item that is both sizeable
and safely removable.

---

## Ranked optimisation candidates for a follow-up session

| # | change | mechanism | est. recovery | isolation property at risk |
|---|---|---|---:|---|
| **1** | **Fix the ASID double-extraction.** Pass `pt->protdom` to `chal_cached_pt_curr`/`chal_cached_pt_update` (or make them take a raw `u16_t asid`). | Restores the intended `CR3_NO_FLUSH` fast path: no flush when the cached pgtbl for that ASID is unchanged — true for every steady-state ping-pong crossing. | **~276 cyc (24 %)** — *done 2026-09-11, `asid_fix.md`* (corrected from ~385 / 34 %) | **Low if done correctly, high if done carelessly.** The fast path skips a TLB flush; if `mapped_pt[asid]` is ever stale (ASID recycled to a new address space, or a pgtbl mutated without a matching `chal_cached_pt_update`) `NO_FLUSH` would skip a *required* flush → stale translation → cross-component memory disclosure. Requires an audit of every `mapped_pt` writer and every pgtbl-mutation path. The as-shipped bug is "safe but slow"; `FORCE_NOFLUSH` (which faults in init, BM5b) shows "fast but unsafe" is a real failure mode. |
| **2** | **De-pack the `thd_invstk_push` copy.** Drop `__attribute__((packed))` from `struct comp_info` (check `cap_sinv` layout), or replace the `memcpy` with an aligned struct assignment. | Byte loop → 4 word moves. | **−31 cyc, measured** (`ipc_path.md`) | **None.** Kernel-internal copy of validated data; only in-kernel layout/ABI assumptions need re-checking. |
| **3** | **Cache the invocation-stack top with the thread** (the TODO already at `inv.h:268`). | Avoids a cache-line access on inv/ret. | ~0 — all remaining bookkeeping is ~19 cyc visible (`ipc_path.md`) | Low. Consistency with the context-switch path is the named concern. |
| 4 | Skip `ltbl_isalive` on the sinv target. | Removes a liveness-table lookup per crossing. | ~10–20 cyc | **High — not recommended.** The liveness check is what stops an invocation into a deallocated component. Pure trade of the guarantee for cycles. |
| 5 | Merge the call+return crossings into one kernel entry. | — | — | **Out of scope.** Breaks the model: the server must run in its own protection domain. Listed as considered-and-rejected. |

**Attack #1 first** — largest single recovery, and it is fixing an outright bug
rather than trading away a property. Budget the follow-up mostly for the
`mapped_pt` / pgtbl-mutation audit, not the one-line change.

---

## Reproduction

Measurement toggles live in `src/platform/i386/chal/shared/cos_config.h`
(all `#ifdef`-gated, all **OFF** in the committed state):

| define | effect | files touched |
|---|---|---|
| `W1_CR3_INSTRUMENT` | H1 hit/miss/ASID counters in `chal_pgtbl_update`; dumped by ping printing `W1CR3` | `chal_proto.h`, `chal.c`, `capinv.c` (`printfn` magic) |
| `W1_CR3_FORCE_NOFLUSH` | **UNSAFE** — `cr3 |= CR3_NO_FLUSH` always | `chal_proto.h` |
| `W1_PROBE` | `rdtscp` probes in `composite_syscall_handler` / `sinv_call` / `sret_ret`; dumped via `W1PROBE`; also enables the null cap | `capinv.c`, `inv.h` |
| `W1_NULLCAP_ENABLE` | cheap bare-crossing cap `0xf00` (returns before dispatch, no `rdtscp`) | `capinv.c` |

`ping.c` reads these via `<cos_config.h>` and adds the matching timed loops +
`W1CR3`/`W1PROBE` dump requests. **None of the instrumentation, and especially
not `W1_CR3_FORCE_NOFLUSH`, should be kept** — it exists only to measure.

Kept-worthy: the `printk` lock and the `crt_clear_schedevents` fix (the latter is
a genuine bug in the no-scheduler boot path).

QEMU iteration: `./cos build && ./cos compose composition_scripts/ping_pong.toml
ping_pong && qemu-system-x86_64 -enable-kvm -cpu max -smp 8 -m 8192 -cdrom
system_binaries/cos_build-ping_pong/cos.iso -no-reboot -nographic -nic none`
(`./cos run` uses `-smp 16`, which also works; 8 matches `NUM_CPU`).

Bare metal: `composite-testrun` skill, `OBS_LOCAL=1`, capture live before boot.
Logs: `/home/syslab/cos-logs/log_ipc-bm{1,2,3,4,5b,6}.txt`.
