# Where the remaining IPC cycles go (post-ASID-fix)

**Date:** 2026-09-14
**Base:** branch `asid_fix` (`efdef3a0` + `4e069fed`) on `main` @ `3ef8f8c4`
**Composition:** `ping_pong.toml` (no capmgr, no scheduler — no timer ticks)
**Where:** bare metal, A03, Xeon Platinum 8160 @ 2.10 GHz, MPK off, PCID on
**Logs:** `/home/syslab/cos-logs/log_ipc-micro.txt`, `log_ipc-fastcopy.txt`, `log_ipc-capmodes.txt`

---

## Headline

The fixed round trip is **853 cycles**, and it decomposes with **no residual**:

| bucket | cycles | share | how measured |
|---|---:|---:|---|
| 2 × `mov %cr3` (with `NO_FLUSH`) + ASID bookkeeping | **512** | **60 %** | `k2 − k0` = 256, ×2 |
| 2 × bare kernel crossing (syscall/sysret, 15-register save+restore) | **~260** | 30 % | clean null syscall = 130 (`baseline.md`, BM6), ×2 |
| invocation-stack `comp_info` copy (packed → byte loop) | **31** | 3.6 % | end-to-end A/B |
| capability checks: lookup + 2 × liveness | **13** | 1.5 % | end-to-end A/B |
| other kernel bookkeeping (`__userregs_*`, bounds checks) | ~5 | 0.6 % | remainder of `821 − 260 − 512` |
| server stub (`custom_acquire_stack`, shuffle, `call pong_call`, body) | **24** | 2.8 % | `sinv-direct call − sinv-direct nop` |
| client trampoline + generic C stub | **8** | 0.9 % | `pong_call − sinv-direct call` |
| **total** | **853** | | measured `pong_call()` |

> **Corrected later on 2026-09-14.** This table first charged the crossings with `k0`
> (145 × 2 = 290), which left only ~19 cycles for all kernel bookkeeping. The copy A/B
> alone then measured −31, exposing the error: `k0` includes the micro-op dispatch (a
> call into `w1_micro` and its switch) that the real path never pays. With the clean
> null syscall (130) the crossings are ~260, and the ~49 cycles of bookkeeping are almost
> entirely accounted for by the two end-to-end A/Bs (copy 31, capability checks 13).

**The user-level stubs are not expensive: ~32 cycles, 3.7 % of the round trip.**
An earlier estimate in `asid_fix.md` put the user side at ~147 cycles; that came
from a *residual* (total minus probe-derived kernel buckets), and it was wrong.
The cycles it was absorbing belong to the CR3 writes, which the perturbed
`rdtscp` probe had badly underestimated (~107 per crossing; the true figure is
~256). Corrected in place there.

**Nothing microarchitectural is going wrong.** Per round trip: **0** branch
mispredicts, **0** i-cache tag misses, **0** page walks. The indirect branches in
the path (`call *INVFN(%rax)` in the client trampoline, `jmp *%rcx` in
`custom_acquire_stack`, `jmp *%rax` in the argsrets server C-stub) are perfectly
predicted in steady state. The cost is *architectural* — a serialising `mov %cr3`
and two syscall/sysret transitions — not a stall or a miss.

---

## Method: additive, not subtractive, and no probes on the real path

Capabilities `0xf00 + N` enter the kernel, run **one** mechanism of the
invocation path, and return (`w1_micro()` in `capinv.c`). `kN − k0` is that
mechanism's cost. The real `sinv`/`sret` path is untouched — the only thing added
to it is one compare-and-branch in the syscall dispatcher.

The user side is peeled from the inside out:

- `sinv-direct nop` — `call_cap_op()` straight to a sinv whose server entry is a
  hand-written `mov $RET_CAP,%rax; syscall` (new `pong_nop`): kernel only.
- `sinv-direct call` — same, to the real `pong_call` sinv: adds the server stub.
- `pong_call()` — the normal path: adds the client trampoline and C stub.

Hardware counters are read with `rdpmc` at the loop brackets (8 GP counters,
perfmon v4; `chal_pmu_setup()` programs them and sets CR4.PCE). PMU programming
is skipped under a hypervisor (CPUID.1:ECX bit 31) because QEMU/KVM advertises
counters whose MSRs `#GP` on this host.

**Two independent checks that the method is sound:**

1. The buckets **sum to the measured whole** (853) without a fudge factor.
2. The TLB event encodings validate themselves: `k1` writes CR3 *with*
   `NO_FLUSH` and `k1f` writes the identical value *without* it. Only that one
   bit differs, and the counters move exactly as they should — 0 → 1 dTLB walk
   and 0 → 3 iTLB walks.

---

## Bare-metal measurements

Median cycles per operation; counters are per-operation (÷100 from the raw log).
`N = 64` samples × 1024 operations. Spread is 0–6 cycles throughout.

| variant | cyc | isolated cost | uinst | kinst | brmiss | dwalk | iwalk |
|---|---:|---:|---:|---:|---:|---:|---:|
| empty loop | 0 | — | 1.0 | 0 | 0 | 0 | 0 |
| `k0` null cap | 145 | baseline | 13.6 | 117 | 0 | 0 | 0 |
| `k1` CR3 write, `NO_FLUSH` | 396 | **251** | 13.1 | 123 | 0 | 0 | 0 |
| `k1f` CR3 write, flushing | 484 | 339 | 13.6 | 122 | 0 | **1** | **3** |
| `k2` real `pgtbl_update()` | 401 | **256** | 13.6 | 139 | 0 | 0 | 0 |
| `k3` `ltbl_isalive()` | 164 | 19 | 13.6 | 132 | 0 | 0 | 0 |
| `k4` dispatch (`invstk_current`+`captbl_lkup`) | 167 | 22 | 13.1 | 152 | 0 | 0 | 0 |
| `k5` packed `comp_info` memcpy | 207 | **40** | 13.1 | **318** | 0 | 0 | 0 |
| `k6` same 32 B as 4 word moves | 165 | ~0 | 13.1 | 153 | 0 | 0 | 0 |
| `k7` invstk push + pop | 204 | 37 | 13.1 | 344 | 0.02 | 0 | 0 |
| `sinv-direct nop` | 821 | — | 15.6 | 458 | 0 | 0 | 0 |
| `sinv-direct call` | 845 | — | 41.6 | 458 | 0 | 0 | 0 |
| `pong_nop()` | 834 | — | 38.0 | 458 | 0 | 0 | 0 |
| **`pong_call()`** | **853** | — | 64.0 | 458 | 0 | 0 | 0 |
| `pong_argsrets()` | 863 | — | 88.0 | 458 | 0.03 | 0 | 0 |

### What the numbers say

- **`mov %cr3` costs ~256 cycles even with `NO_FLUSH`.** It is a serialising,
  microcoded instruction; the ASID bookkeeping around it (extract, cache check)
  is only ~5 (`k2 − k1` with `k1`'s extra CR3 *read*). Two per round trip.
- **A flush on top of that adds ~88** in this micro-benchmark (`k1f − k1`). Note
  this *understates* the real flush: `k1f` only re-walks the null path's tiny
  working set (4 walks), whereas a real crossing re-walks the destination
  component's. That is why the ASID fix recovered ~276 rather than ~176.
- **The packed copy is 166 kernel instructions** (32 iterations × ~5) for 32
  bytes — 74 % of all IPC-specific kernel instructions, and 40 cycles isolated.
- **Isolated costs over-sum.** The mechanisms measured alone total ~97 cycles of
  bookkeeping, but only ~49 are visible in the round trip (31 for the copy and 13
  for the capability checks, both measured end-to-end, plus ~5). Work placed
  *before* the serialising CR3 write partly hides behind its pipeline drain. Any
  estimate of a saving from trimming this bookkeeping must be discounted
  accordingly — which the A/Bs confirm.

### A/B: replacing the packed byte-loop copy (measured, not estimated)

Same build, one change — `thd_invstk_push()` copies the 32 bytes as four word
moves instead of the packed `memcpy`:

| | byte loop | word moves | delta |
|---|---:|---:|---:|
| `k7` invstk push+pop | 204 | 171 | −33 |
| `sinv-direct nop` | 821 | 791 | −30 |
| **`pong_call()`** | **853** | **822** | **−31** |
| `pong_argsrets()` | 863 | 830 | −33 |

**~31 cycles (3.6 %), safe, no isolation impact.** Less than the 40 isolated, as
predicted by the overlap effect above. This is a ready-to-land patch.

---

## Capability checks: 13 cycles (1.5 %)

Before it switches address spaces, the invocation path makes two kinds of check:

- **Capability lookup** (sinv only). `thd_invstk_current()` finds the caller's
  capability table, `captbl_lkup()` walks it for the invoked id, and the header is
  checked for an allocated slot of type `CAP_SINV`. This *is* the access control: a
  component can invoke only what its table grants. In the linked kernel it is ~45
  instructions and 6–7 branches — two loads for the invocation stack, one of them
  from the **user-level** invocation stack (`ulk_invstk->top`, MPK-era state consulted
  even with MPK off), one for the table pointer, a 2-level radix-trie walk (internal
  node, then leaf aligned to the cache line), and two byte loads of the header.
- **Liveness checks** (sinv *and* sret). `ltbl_isalive()` compares the epoch cached in
  the capability against the liveness table, so the kernel never enters — or returns
  into — a component that has been torn down. ~8 instructions each: two loads from the
  capability, a bound check, one load from the liveness table, a compare.

### Measured inside the round trip

One image, one boot, identical code layout. An UNSAFE run-time switch (micro op 13)
skips the checks during ping's single-threaded loop only; checks-on runs bracket the
variants to catch drift, and the full API is re-asserted after the checks are restored.
Bare metal, median of 64 × 1024:

| mode | `pong_call()` | Δ | kernel instrs | Δ | branches | Δ |
|---|---:|---:|---:|---:|---:|---:|
| checks ON | 854 | — | 480 | — | 81.5 | — |
| no liveness (both checks) | 849 | **−5** | 462 | −18 | 77.5 | −4 |
| no capability lookup | 844 | **−10** | 451 | −29 | 76.5 | −5 |
| no liveness + no lookup | 841 | **−13** | 433 | −47 | 72.5 | −9 |
| checks ON (again) | 854 | — | 480 | — | 81.5 | — |

`sinv-direct call` moves by the same amounts (842 → 838 / 833 / 830). The brackets are
identical, spreads are 1–2 cycles, and the two savings roughly add (5 + 10 ≈ 13).

**All capability checking costs ~13 cycles of an 854-cycle round trip.** Measured in
isolation the same work is ~60 (`k3` ≈ 19 per liveness check and `k4` ≈ 22 for the
lookup, in the earlier image); inside the round trip about four-fifths of it hides
behind the serialising CR3 write. The lookup's true figure is slightly above 10: the
"no lookup" mode still pays a per-cpu cache compare (~5 instructions) in place of the
~34-instruction lookup.

These checks are the isolation *policy* — who may call whom, and never into a dead
component — and they are 1.5 % of the path. The *mechanism* that enforces isolation,
two page-table switches, is 60 %. Weakening the checks buys nothing worth having; the
refinements that stay safe are worth a few cycles at most:

- compile the `ulk_invstk` test out of `thd_invstk_current()` when MPK is off (one load
  from the ULK page and a compare on every sinv): ~1–3 cycles, safe;
- a properly keyed capability cache (caller's table, capability id, revocation epoch):
  at most the ~10 of the lookup, with real revocation-consistency risk. Not recommended.

In this image the isolated micro-ops did not subtract cleanly (`k3` 153 and `k4` 153
against `k0` 157) — most likely a code-layout effect of the mode switch on the tiny
`w1_micro` path. That is why the in-round-trip A/B above, not `kN − k0`, is the
measurement to trust here. Log: `log_ipc-capmodes.txt`.

---

## Ranked candidates for further optimisation

| # | change | mechanism | measured / estimated | isolation risk |
|---|---|---|---:|---|
| **1** | **Stop switching page tables**: the shared-VAS + MPK path already in the tree (`MPK_ENABLED`, `addrspc_shared`, `alt_fn` → `__cosrt_fast_callgate_*`, `mpk_jit`). | Replaces two ~256-cycle `mov %cr3` with `wrpkru` (~20–30), and the user-level callgate skips the kernel entirely, removing the 290 of crossings too. | **up to ~800 of 853** — not yet measured | **High, and a different model.** Protection keys, not page tables: 16 keys, `wrpkru` is unprivileged, so isolation rests on the JITed callgate and its auth tokens rather than on hardware address-space separation. Needs its own study. |
| **2** | **De-packed `comp_info` copy** (patch above). | 166 kernel instructions → 4 moves. | **−31, measured** | **None.** Kernel-internal copy of already-validated data; identical bytes. |
| 3 | **Lighter register save on the IPC crossing.** `SAVE/RESTORE_REGS_ALL` pushes and pops 15 GPRs; the sinv ABI does not need all of them preserved. | 117 kernel instructions per crossing, ~260 cycles for two. | ~10–20, estimated | Moderate: the sinv/sret register contract and the fault/upcall paths both depend on the saved frame. |
| 4 | Capability checks: lookup + 2 × liveness (see *Capability checks*). | ~34 + 2 × ~8 kernel instructions. | **−13 if removed entirely, measured** | **They are the access control.** Removing them lets a component invoke anything, or enter a torn-down one. Only the MPK-only `ulk_invstk` test is safely removable (~1–3). |
| 5 | `pong_argsrets`'s extra server C-stub hop (`__cosrt_s_cstub_*` is a `movabs; jmp *%rax` trampoline). | One indirect jump + a C frame. | ~8–10 | None. Cosmetic. |

**The honest summary: after the ASID fix, ~90 % of the round trip is two CR3
writes and two kernel crossings.** Everything else — both user-level stubs, the
invocation-stack copy, the capability checks — is ~80 cycles combined, and of that
only the copy (31) is a safe, worthwhile cut. There is no
further micro-optimisation of consequence; the remaining lever is architectural
(#1), and it is a different isolation model rather than a free win.

---

## Not verified

- The MPK/shared-VAS path (#1) is **not measured** — the estimate is arithmetic
  from `wrpkru` vs `mov %cr3`, not an experiment. It needs `MPK_ENABLED`, a
  shared-VAS composition, and its own correctness check.
- `mov %cr3`'s ~256 cycles is measured but not *explained* at microarchitectural
  level (how much is the serialising drain vs the PCID-tagged reload). No
  counter here separates those.
- One byte-loop `comp_info` copy remains in the kernel outside the sinv fast path
  (`cmp $0x20,%rax` still present once in the handler after the A/B); not chased.
- `k5` moved 207 → 200 between the two boots with an unchanged code path,
  probably capability-table layout (cap numbers differed: 88 vs 92). Run-to-run
  variation on identical code is therefore ~7 cycles for the `kN` family, even
  though within-run spread is 0–6.
- QEMU/KVM is unusable for this decomposition: `mov %cr3` traps there, so
  `k1`/`k2` read ~250 under KVM for the wrong reason. All numbers above are bare
  metal.

## Tree state

Measurement scaffolding only, all `#ifdef`-gated and **not** committed:
`W1_MICRO` (the `0xf00+N` caps and `chal_pmu_setup()`), `W1_FASTCOPY` (the copy A/B),
`W1_CAPMODES` (the UNSAFE capability-check switches),
the `pong_nop` interface function and its hand-written server stub, and the
`ping.c` harness. The branch `asid_fix` contains none of it.
