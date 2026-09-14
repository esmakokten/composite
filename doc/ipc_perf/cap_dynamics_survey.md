# Adding and removing capabilities at run time

**Date:** 2026-09-14
**Tree:** branch `asid_fix` (`efdef3a0` + `4e069fed`) on `main` @ `3ef8f8c4`
**Scope:** capability tables and the capability state the kernel caches, on x86-64
with MPK off. MPK user-level callgates are out of scope.
**Companion to:** [`tlb_flush_survey.md`](tlb_flush_survey.md) — the same question
("what goes stale when something is removed?") asked about page tables.
**Sources:** audit of the Composite source. Line numbers are given only for files the
measurement scaffolding has not touched (`captbl.h`, `captbl.c`, `cap_ops.h`,
`component.h`, `liveness_tbl.h`, `ipi_cap.h`); elsewhere, function names. seL4 is
described from knowledge, for comparison.

---

## TL;DR

**Removal uses two mechanisms, solving different problems:**

1. **Slot quiescence — RCU with a time-based grace period.**
   - `captbl_del` stamps a timestamp and marks the slot `CAP_QUIESCENCE`.
   - `captbl_add` refuses to reuse that slot for `KERN_QUIESCENCE_CYCLES` (~1 ms here).
   - This protects kernel code on other cores that is part-way through a lookup. It is
     **sound**: capability tables are walked only by kernel code inside one syscall,
     unlike page tables, which the hardware caches (the TLB survey's hard case).
2. **Liveness epochs — object-level revocation.**
   - Components, sinv capabilities, invocation-stack frames and asnd capabilities each
     cache a copy of a component's `(liveness id, epoch)`.
   - `comp_deactivate` bumps the epoch, which invalidates **every cached copy at once**:
     every sinv into the component in every table (including copies made by `cap_cpy`),
     every return into it, every asnd to it.
   - Only components use this.

**No capability derivation tree.**
- `cap_cpy` copies the capability body; removing one copy leaves the others working.
- "Revoke everything I delegated" is left to user level. seL4 does it in the kernel,
  with its capability derivation tree and `Revoke`.

**Adding** needs no invalidation. But activation publishes a capability's type before
its body is written (the authors' `FIXME`s), so a concurrent invocation can see a
half-built capability — or the previous occupant's body.

**Four removal gaps.** All latent: nothing in the tree removes a capability at runtime.

1. **Async receive endpoints have no epoch** (hard-wired to 0). A stale asnd reaches
   whatever arcv later reuses the slot. The cross-core path checks neither the epoch nor
   the receiving component's liveness.
2. **A thread already running inside a deactivated component keeps using that
   component's capabilities.** Syscall entry skips the current component's liveness
   check by design.
3. **That component's top-level capability table can be freed while the thread still
   uses it, as soon as TLB quiescence works.** Today it can never be freed — fail-closed,
   a leak. This directly constrains the TLB survey's recommendation #2.
4. **User-level stubs cache capability ids.** After slot reuse, a stale name invokes a
   different capability. That is a naming hazard, not privilege escalation: the client
   holds the new capability anyway.

**Fast-path cost of fixing:** only optional per-capability revocation and one of the
two eviction options touch the sinv fast path — each one load and compare, roughly
2–3 cycles, going by the 5 cycles measured for two liveness checks in `ipc_path.md`.

---

## 1. The machinery

### 1.1 Capability slots: publish and unpublish

- **`captbl_add`** (`captbl.h`) reserves the slot's allocation-map bit in the cache-line
  header, then checks reuse:
  - a slot previously deactivated (`CAP_QUIESCENCE` with a liveness id) is reused only
    if `QUIESCENCE_CHECK(now, ltbl_get_timestamp(lid), KERN_QUIESCENCE_CYCLES)` passes;
  - changing a cache line's entry size requires *every* entry in that line to be
    quiescent (with a `FIXME` about racing a concurrent resize).
- **`captbl_del`** (`captbl.h:378`):
  - `ltbl_timestamp_update(lid)` first, so the reuse check sees a real deactivation time;
  - then sets the slot to `CAP_QUIESCENCE` with `liveness_id = lid`;
  - memory fence, then clears the allocation bit.
- **The liveness id is supplied by the caller** (user level). Stamping the same id again
  only lengthens other slots' grace periods — the safe direction.
- **The grace period** is `KERN_QUIESCENCE_PERIOD_US` = 500 µs, converted at a
  hard-coded 4000 cycles per µs: 2 M cycles, ≈ 0.95 ms on this 2.10 GHz part (longer
  than intended; safe). It assumes no syscall walking a capability table outlasts it —
  plausible, with interrupts off and no preemption in the kernel.

### 1.2 Liveness epochs: object-level revocation

`liveness_tbl.h`: each id has an `epoch` and a `deact_timestamp`.
- `ltbl_get` (`:144`) snapshots the current epoch into a copy.
- `ltbl_expire` CAS-increments the epoch and stamps the timestamp.
- Epochs never reset after boot, so reusing an id cannot revive a stale copy.

**Where copies of `(id, epoch)` live:**

| holder | copied when |
|---|---|
| component capability, `info.liveness` | `comp_activate` |
| sinv capability, `comp_info` (target component) | `sinv_activate` (`memcpy`) |
| thread invocation-stack frame, `comp_info` | every sinv (`thd_invstk_push`) |
| asnd capability, `comp_info` (receiver component) | `asnd_construct`, from the arcv |
| cross-core IPI message, `comp_info` | `ipi_cap.h` send path |

**Where they are checked:**

| path | check | on failure |
|---|---|---|
| sinv into a component | `sinv_call`: target epoch | `-EFAULT` |
| return into a component | `sret_ret`: caller epoch | `-EFAULT` |
| switch *to* a thread | `cap_thd_switch`: that thread's current component | `-EFAULT` (`FIXME`: should fault the thread) |
| local asnd delivery | `__cap_asnd_to_arcv`: receiver component | refuse |
| **any syscall** | **current component — not checked** | comment: "guaranteed by component quiescence period, which is at timer tick granularity" |
| **asnd → arcv identity** | **not checked** — `arcvc->epoch = 0; /* FIXME: get the real epoch */` | — |
| **cross-core asnd delivery** | **neither** — `cos_ipi_arcv_get` (`ipi_cap.h:74`): "FIXME: check epoch and liveness!" | type check only |

**Only `comp_deactivate` expires an epoch.** Epochs revoke *components*, not individual
capabilities.

### 1.3 References that pin memory

- **Component capabilities hold references** on the component's top-level page table and
  capability table. `comp_deactivate` (`component.h`) expires the epoch and then **drops
  both references immediately**.
- **`cap_cpy`** (`capinv.c`) takes a reference only for threads (`thd->refcnt++`) and
  capability tables (parent refcount). Sinv, asnd and other capabilities are plain byte
  copies.
- **`captbl_decons`** (`captbl.c:159`) clears the two half-page leaf entries and drops
  the child's refcount. No quiescence at unlink time.
- **Freeing a capability-table page** (`kmem_deact_pre`):
  - lower levels: kernel quiescence (time) plus a scan checking every entry is quiescent
    (`captbl.c`). **Sound** — only in-syscall kernel walkers can hold pointers in.
  - top level: **TLB** quiescence — "top level has tlb quiescence period (due to the
    optimization to avoid current_component lookup on invocation path)". Threads keep a
    long-lived pointer to the top-level table in their invocation stack. On x86-64 this
    check never grants, so **top-level capability tables are never freed.**
- **arcv:** `arcv_deactivate` refuses while its thread is referenced as a receiver, but
  asnd capabilities take no reference on the arcv.

---

## 2. Adding capabilities

- **No invalidation needed.** A lookup either misses or finds the new capability — the
  analogue of a not-present → present page-table entry.
- **Reusing a slot** waits out the grace period (§1.1).
- **Activation is not atomic.**
  - The intended protocol is three steps: (1) reserve the slot, (2) write the body,
    (3) publish the type.
  - `captbl_add` does (1) and (3) together — *"FIXME: we should _not_ do this here. This
    should be done in step 3 of the protocol for setting capabilities, not 1"* — and
    `__cap_capactivate_post` (`cap_ops.h:33`) notes the missing store barrier.
  - Callers such as `sinv_activate` write the body (`token`, `comp_info`, `entry_addr`)
    only after `captbl_add` returns.
  - So for a few instructions, a lookup on another core can get a slot typed `CAP_SINV`
    whose body is either the **previous occupant's** — for a reused slot; if that old
    target is still alive, the liveness check passes and the call enters the old server
    — or **not yet written**. Not traced to a concrete failure (see *Not verified*).
- **Runtime additions in-tree:** crt aliasing sinvs, threads and receive endpoints into
  components (`cos_cap_cpy`, `cos_cap_cpy_at`), and capability managers creating threads
  and endpoints.

---

## 3. Removing capabilities: what each removal leaves behind

### 3.1 Overview

| removed | new use | cached state left behind | checked? | gap |
|---|---|---|---|---|
| **a sinv capability** from a client's table (`SINVDEACTIVATE`) | the next lookup sees `CAP_QUIESCENCE` and the type check rejects it — immediate | copies made by `cap_cpy` in other tables; threads already inside the server (returns use the invocation stack, not the capability); the client stub's cached id | copies: by design. Stub: no | **Naming hazard** — once the grace period passes, the id can be reused and the stale stub invokes whatever sits there: authority the client holds, but not the capability it means |
| **a component** (`COMPDEACTIVATE`) | epoch bump: sinvs in, returns in, switches to its threads, and local asnds to it all fail | a thread *currently running* inside it | **no** — syscall entry skips the current component | **Eviction not enforced** (§3.2) |
| that component's **top-level capability / page table** (refs dropped by `COMPDEACTIVATE`) | release gated by TLB quiescence | the running thread's invocation-stack pointer to the table | — | **Today: never freed (leak). Once TLB quiescence works: use-after-free** (§3.2) |
| **an arcv** (`ARCVDEACTIVATE`) | refused while its thread is referenced | asnd capabilities naming its slot, with `arcv_epoch = 0` | **no epoch check**; the cross-core path checks neither epoch nor liveness | **Wrong receiver** — once the slot holds another arcv, stale asnds notify the new receiver and move time between the wrong tcaps; cross-core delivery can target a dead component |
| **an asnd** | lookup rejects | — | — | — |
| **a capability-table subtree** (`DECONS`, then deactivate) | lower levels freed after kernel quiescence plus a full scan | only in-flight kernel walkers | yes (time) | none — sound for software readers |
| **delegated copies** in general | removed one by one | no link between copies | — | By design: "revoke everything derived from X" needs user-level bookkeeping, or an epoch |

### 3.2 A thread inside a deactivated component

1. Thread T is running in component C. Its top invocation-stack frame caches C's
   `comp_info`, including C's top-level capability table.
2. `comp_deactivate(C)` expires C's epoch and drops C's references on that capability
   table and page table.
3. T's next syscalls look up capabilities in C's table **with no liveness check** on
   entry. T can keep invoking C's sinvs into *live* servers — C's authority outlives C —
   until T is switched away. From then on `cap_thd_switch` refuses to resume T.
4. **Memory safety today:** releasing C's top-level capability table requires
   `tlb_quiescence_check()`, which on x86-64 never grants. The table is never freed:
   no use-after-free, just a leak.
5. **Memory safety after the TLB fix:** once quiescence can grant — the TLB survey's
   recommendation #2 — the table can be frozen, freed, zeroed and reused while T is still
   inside C. T's next syscall then walks a freed kernel page.

The comment on the syscall path assumes "component quiescence at timer-tick granularity":
that every thread in C reaches a point that notices before the table goes away. Nothing
enforces that T leaves C.

### 3.3 For comparison: seL4

seL4 keeps a capability derivation tree. `Revoke` removes every capability derived from
a given one, and deleting the last capability to an object finalises it — a destroyed
thread, for example, is stopped as part of that. Invocations re-read the capability each
time, so no cached copy outlives its slot.

Composite trades those guarantees for a cheaper fast path. It caches the target's
`comp_info` in the sinv capability, and all capability checking costs 13 cycles per
round trip (`ipc_path.md`). The price is weaker removal semantics:
- revocation is per object (epochs) rather than per capability;
- per-capability revocation of delegated copies is user level's job;
- the current component is trusted to still exist.

---

## 4. What would close the gaps

| # | change | fixes | fast-path cost |
|---|---|---|---|
| 1 | **Give arcvs a real liveness id and epoch**, and check it in `__cap_asnd_to_arcv` and `cos_ipi_arcv_get`; also check the receiving component's liveness on the cross-core path. | Wrong-receiver delivery; cross-core delivery into dead components (all three existing `FIXME`s) | None on sinv; one load and compare per asnd |
| 2 | **Make activation atomic:** write the body, fence, *then* publish the type and allocation bit (step 3 of the protocol). For reused slots, scrub the body first. | Half-built and previous-occupant capabilities | None |
| 3 | **Before restoring TLB quiescence, stop threads from outliving their component's tables.** Either (a) check the current component's liveness on syscall entry, or (b) have `comp_deactivate` defer dropping the `ct_top`/`pgd` references until no invocation stack names the component — faulting or evicting those threads first. | Eviction; the latent use-after-free | (a) one liveness check per syscall, ~2–3 cycles. (b) none on the fast path |
| 4 | **Capability-id reuse policy in capability managers:** never reuse an id a client still names, or update that client's `usr_inv_cap` when reusing it. | Stale user-level names | None — policy |
| 5 | *Optional:* **per-capability revocation.** Give each sinv its own liveness id, expired on revoke, checked in `sinv_call` alongside the target's. | Revoking one capability *and all its copies* | One more load and compare per sinv, ~2–3 cycles |
| 6 | **Hold the source slot in `cap_move`** — a placeholder, or quiescence — so it cannot be reused before the move commits. | The slot-reuse race its own `FIXME` describes | None |

**Order:** #3 is the one to do **before** the TLB survey's quiescence fix, because that
fix would turn today's leak into a use-after-free. #1 and #2 are independent and cheap.

---

## Not verified

- **Nothing in the tree removes a capability at runtime:** no user-level API issues any
  `*DEACTIVATE` operation. None of these paths has been exercised; every gap here comes
  from code reading.
- **The activation race** (§2) rests on the authors' `FIXME` and the write order in
  `sinv_activate`. It was not reproduced, and what a never-used slot's zeroed body would
  do was not traced through `ltbl_isalive` for liveness id 0.
- **Thread, tcap and hardware-capability removal** was not audited beyond the refcounts.
- **The grace period's premise** — no capability-table walk outlasts ~1 ms of kernel
  execution — is assumed, not measured.
- **MPK user-level callgates** embed invocation authority in JIT-compiled user code, which
  kernel-side removal cannot revoke. Out of scope.
- **The seL4 comparison** is from knowledge.
