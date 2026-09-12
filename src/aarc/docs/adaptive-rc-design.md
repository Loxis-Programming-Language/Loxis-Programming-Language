# AARC v2 design: adaptive inline/asynchronous reference counting

Status: design (not yet implemented).
Related: `../Todo_AddGC.md` (v1 spec, phases 0-2 implemented), `../bench/`.

## 0. Motivation — why this design exists

The current AARC is *uniformly asynchronous*: every mutator reference mutation is
queued to one background worker, and every object pays the async tax.  Measured
on this machine (Release, single thread, acyclic, see
`../bench/bench_local_fastpath.cpp`):

```
ARC        (inline atomic RC)        ~19-20 M obj/s
AARC-sync  (same engine, inline)     ~24-25 M obj/s   (119-133% of ARC)
AARC-async (queue + worker)           ~3.8-4.9 M obj/s (20-24% of ARC)
```

Two conclusions:

1. **The AARC accounting/cycle model is not slow.**  Run inline on one thread it
   is already *faster* than atomic reference counting (no atomics, and it still
   collects cycles).  The entire multi-x gap is the async machinery — enqueue,
   worker drain, notification, sweeps.
2. So forcing every object through the async path is the real design error.

This motivates an *adaptive* split that keeps the fast, correct, inline engine
for the (huge) class of single-owner objects and only moves objects to the async
engine when they actually cross a thread.

## 1. Regimes

Every cell is in exactly one regime:

| regime  | meaning                                      | engine that owns it            |
| ------- | -------------------------------------------- | ------------------------------ |
| LOCAL   | reachable only from one owner thread         | owner's *inline* engine (sync) |
| PROMOTING | ownership hand-off in progress (transient) | none (frozen)                  |
| SHARED  | reachable from ≥ 2 threads (or escaped)      | the async worker engine        |

Naming note: LOCAL is *not* "dumb ARC".  It is the full AARC engine (`AarcEngine`
with cycle collection) running inline.  Only the *execution model* differs:

```
LOCAL  : process(ref-op) runs on the owner thread, synchronously   (= Mode::Sync today)
SHARED : ref-op is queued and drained by the background worker     (= Mode::Async today)
```

The thesis: the same cycle-safe semantics, execution chosen per-object by whether
it is single-owner or shared.

## 2. Design constraints (hard rules)

These are non-negotiable:

1. **One authoritative state.**  At any instant exactly one engine owns the
   refcount/edge truth for a cell — its LOCAL engine if LOCAL, the SHARED worker
   if SHARED, and *nobody* during PROMOTING.  There is never "a copy on each
   side, reconciled later".
2. **Cycles are not a promotion trigger.**  Every owning engine already collects
   cycles (inline for LOCAL).  A single-owner cyclic graph must be handled
   locally, without ever entering the async path.
3. **The only promotion trigger is first cross-owner escape.**  A reference to a
   LOCAL cell from a thread that is not its owner.
4. **No premature reclamation.**  During PROMOTING the cell is frozen: neither
   the LOCAL owner's sweep nor the SHARED worker may reclaim it.
5. **No demotion in v1.**  `LOCAL -> PROMOTING -> SHARED` only.  Correctness
   first; downgrade is a later refinement.
6. **Conservative propagation in v1.**  An escape promotes the *undirected
   connected component* (closure over LOCAL edges) containing the escaped cell,
   so no LOCAL<->SHARED edges survive inside one arena.  Finer-grained / proxy
   promotion is future work.

## 3. Cell / handle identity

**Cells never move; handles stay valid.**  Design choice for v1: one shared
`CellPool` (fixed slot space, versioned handles `{index, generation}`), and the
regime is a per-slot attribute.  Promotion is therefore an *ownership hand-off*,
not a copy: the promoted cells keep their `{index, generation}`, so every handle
any thread already holds remains valid.  This sidesteps the classic
"promotion must rewrite every handle" trap.

Refcount / edge bookkeeping also lives in one shared `RefTable` (indexed by
slot), but each slot is written by exactly one engine at a time (its regime's
owner).  The *scratch* state that must not be shared between engines — zero
candidates, suspect set, Tarjan scratch — is per-engine, and the per-engine
invariant that keeps that safe is:

> **Regime monotonicity of edges:** all outgoing edges of a LOCAL cell point to
> LOCAL cells, and all outgoing edges of a SHARED cell point to SHARED cells.
>
> (Guaranteed by rule 6: promotion closes over the undirected component, so a
> promoted cell never retains an edge to a still-LOCAL cell, and a LOCAL cell
> never gains an edge to a SHARED one.)

Consequences:
* A LOCAL sweep's cascade (zero -> free -> decrement children) only ever touches
  LOCAL slots -> per-engine zero/suspect scratch never crosses regimes.
* LOCAL cycle detection stays within the owner's own subgraph.
* The SHARED worker never sees or reclaims LOCAL slots.

## 4. Cell lifecycle

```
        alloc on thread T
              |
              v
        [LOCAL, owner=T]  ----------------------+
              |                                  | first reference from thread U != T
              |  (all ops inline on T)           v
              |                           [PROMOTING]
              |                                  |  ownership hand-off completes
              |                                  v
              |                           [SHARED, owner=worker]
              |                                  |
              +---- never downgrades -------------+
```

State is monotonic (rule 5).

## 5. Escape detection

Escape is a *concrete, observable event* — no acyclicity oracle needed:

* a `rootAdd` / `rootRemove` on a cell whose regime is LOCAL and whose caller is
  not the recorded owner, or
* an `edgeAdd(source, target)` where source and target regimes differ (this is
  the forbidden cross-regime edge; it must be promoted, not allowed to exist).

Because rule 3 says only escape promotes, and escape events are rare in
well-written single-owner code, LOCAL objects never poll a worker and never pay
message costs.

## 6. The promotion protocol

Promotion must move a connected component from "owned by thread T (inline)" to
"owned by the SHARED worker".  The owner T is the *only* writer of its LOCAL
cells, so promotion must run while T is not mutating them.

### 6.1 Initiator

Thread U (U != T) that wants to reference a LOCAL(T) cell X:
1. U **requests** promotion of the component containing X through a per-owner
   control channel (an atomic flag + the escaping handle(s)).
2. U **blocks** until T has completed the hand-off and U is handed back a
   SHARED handle (still the same `{index, gen}`), then U proceeds with a normal
   SHARED (async) op.

### 6.2 Owner-side hand-off (two-phase: worker adopts BEFORE SHARED is visible)

Thread T notices the request at one of its inline check points (a cheap,
periodic poll on T's local op entry — a *local safepoint*, not a global stop).
Because T is single-threaded over its cells, between two local ops the LOCAL
graph is quiescent.  The ordering below guarantees `SHARED` is never observable
before the worker is ready for it:

1. T discovers the closure: the undirected connected component (rule 6)
   containing the escaped cell.
2. T marks every member `LOCAL -> PROMOTING`.  From now until commit T skips
   these cells in its own sweeps (rule 4).  No other writer exists, so this is
   an in-place flip, not a stop-the-world.
3. T freeze/snapshots the component and *stops* mutating it.  Its RefTable rows
   already are the truth (rule 1) — nothing is recomputed or copied.
4. T sends `AdoptComponent(component)` to the SHARED worker.
5. Worker adopts: it takes ownership of the *existing* RefTable rows and
   rebuilds its own ephemeral zero/suspect/Tarjan scratch for those slots
   (see 6.2.1).  It replies **ACK**.  Normal SHARED ops are still not allowed
   on these slots.
6. **Only now** does T publish the members `PROMOTING -> SHARED`.  Because the
   members flip one by one, this publication must not be visible piecemeal to
   the worker: T wraps the whole component in one commit.
7. T issues `CommitPromotion(componentEpoch)`.  After this commit gate the
   worker may start processing normal SHARED ops on the component.
8. T unblocks U and any other waiters (they proceed with normal SHARED ops).

Why adopt-before-publish: if members flipped `SHARED` first and the worker
adopted later, a third thread already holding a handle could see `SHARED` and
issue a normal op inside the window before adoption — racing the adoption.
Publishing only after the worker is ready closes that window.  The commit gate
closes the second window: the worker must not start on member #10 while members
#11..#N are still `PROMOTING`.

#### 6.2.1 Promotion transfers ownership, never data

RefTable is one shared table.  Promotion does **not** copy rows and does **not**
replay edges as worker-side `EdgeAdd`s — replaying an already-present graph
would double-count references.  Concretely, worker adoption means only:

* ownership of the slots' rows passes from T's engine to the worker's engine
  (same RefTable rows, so nothing moves, nothing is rebuilt),
* the worker (re)initializes its per-engine ephemeral state for those slots:
  zero candidates, suspect set, Tarjan scratch,
* if any promoted slot already has refcount 0 it is added to the worker's
  zero-candidate set so it can be reclaimed on the next drain.

The authoritative rows are never re-derived; only *who writes them* changes,
plus per-engine collector scratch is (re)built on the adopting side.

### 6.3 Why this is correct (sketch)

* Freeze: between T's local ops the graph is stable; PROMOTING cells are
  excluded from T's sweeps, and the worker has not seen them yet, so nobody can
  reclaim them (rules 3,4).
* Single authority transfer: ownership of the (never-copied) rows passes from T
  to the worker at commit.  Before commit T is the only writer; after commit
  only the worker writes those slots.  Since the rows are the same memory there
  is no instant where two writers can disagree on a count.
* Commit gate: the worker starts processing the component only after every
  member has been published and the component epoch committed — no partial-
  component races.
* Component closure (rule 6): after promotion there are no LOCAL<->SHARED
  edges, so each engine's sweep/cycle-collection stays inside its regime and
  the per-engine scratch invariant holds.
* Handles: unchanged `{index, gen}` throughout, so no handle rewrite and no
  ABA (generation still guards reuse).

### 6.4 Quiescence requirement

Before the worker may reclaim anything in the promoted component, the usual
v1 drain rule applies (all queues drained up to a common snapshot).  Existing
worker machinery is reused unchanged; the new parts are only the ownership/
scratch adoption and the commit gate.

## 7. What must change in the code (v1 milestones)

Current code: one `CellPool`, one `AarcEngine`, `Runtime` has Sync (inline) and
Async (worker) *modes*, and every op chooses a mode at Runtime level.  v1 needs
regime *per cell* and both engines live simultaneously.

* **M1 — regime state machine + escape plumbing (no promotion yet).**
  - Add a per-slot regime with **all three states from day one**: `LOCAL`,
    `PROMOTING`, `SHARED` (enum in `CellPool`; the owner id already exists as
    `owner_`).  `PROMOTING` is present in M1 because the promotion-race
    invariant depends on it — do not defer it to M3.
  - The regime exists but is not yet exercised: until M2 splits engines, the
    current single-worker runtime treats every cell as `SHARED`.
  - Per-owner control channel (request flag + handle) and the owner's periodic
    local-safepoint poll.  Instrument only; nothing is promoted yet.
  - Assert invariant at every quiesce: no cross-regime edges exist.
* **M2 — inline engine on the owner thread + regime/ownership invariant checker
  (hard completion gates).**
  - Allow one `AarcEngine` (sync process/sweep) per owner over LOCAL slots while
    the worker concurrently serves SHARED slots; RefTable is shared but slot
    ownership is disjoint.
  - The invariant checker is a **hard completion condition**, not a risk note.
    In a checked build it aborts on any of:
    * a LOCAL engine touched a SHARED slot,
    * the SHARED engine touched a LOCAL slot,
    * a cross-regime edge at any stable point,
    * a `PROMOTING` slot was reclaimed,
    * a thread other than the current owner wrote a slot's RefTable row.
  - Validate: single-threaded LOCAL workload == `AARC-sync` throughput (the
    ~25M ceiling) with both engines alive in one process and no cross-talk.
* **M3 — promotion hand-off.**
  - Implement §6 end to end: escape detection -> request -> owner freeze ->
    component closure -> regime flip -> worker adoption -> unblock U.
  - Kill-criteria tests: no loss/double-free/cross-regime edge under stress;
    handles stable; cyclic LOCAL graphs handled inline (never promoted solely
    due to cycles); escaped components reclaimed by the worker.
* **M4 — benchmarks.**
  - Extend `bench_compare`: (i) pure thread-local workload should track
    `AARC-sync`/ARC (not async), (ii) 30% shared-escape workload should stay
    correct with live=0, (iii) the T1 ugly number should collapse to ~inline
    because single-thread acyclic never escapes.

## 8. Explicitly out of scope for v1 (documented to avoid silent assumptions)

* **Demotion** (`SHARED -> LOCAL`).  Monotone up only.
* **Finer-grained / proxy promotion.**  Component closure can promote more than
  strictly necessary; acceptable.  Boundary objects and cross-regime edge
  proxies are future work.
* **Global stop-the-world.**  Freeze is owner-local via the local safepoint.
* **Fragmented per-thread arenas / migration-by-copy.**  We chose single-pool
  + ownership hand-off to keep handles stable.  If per-owner arenas are later
  preferred for cache locality, the copy/rewrite cost must be re-derived.
* **Multi-worker SHARED regime.**  The SHARED engine is still the current single
  worker; multi-worker is an orthogonal optimization underneath the shared
  regime, not part of the adaptive split.

## 8.5 Claim boundary (paper wording)

The design has **no global stop-the-world** — only the owner freezes at its own
local safepoint, and other mutators never stop.  It is nevertheless **not
pause-free**: promoting a large component pauses the owner for roughly
O(component size) and blocks the requester U until commit.  Word the paper as
*no global STW*, never *pause-free*.  Worth measuring later: a
`promotion component size -> owner pause latency` curve as its own figure.

Suspect pressure is measured as the number of currently marked distinct
suspects, not the size of the lazy work stack.

## 9. Open risks (to resolve during M1/M2)

1. **Owner liveness.**  If owner T never polls (T died / is blocked forever), a
   promotion request from U deadlocks.  v1 assumes owners poll at local ops and
   are alive; a timeout/fallback policy is TBD.
2. **RefTable sharing.**  Disjoint slot ownership must be airtight; a single bug
   where a LOCAL sweep touches a SHARED slot corrupts both regimes.  M2 should
   add an invariant checker (assert no LOCAL-owned sweep crosses regime).
3. **Escape while PROMOTING.**  Two threads racing to escape the same component:
   the protocol must collapse both requests onto one hand-off (the second just
   waits on the same completion signal).
4. **Roots held outside the engine.**  This design assumes all references are
   engine-visible (roots and edges recorded in RefTable).  References held in
   raw mutator stack/registers that are never registered are out of the engine's
   view today; the VM-binding layer must treat those as roots via the existing
   root API before this split can be trusted end to end.
