# AARC — Asynchronous ARC (experimental GC engine)

An experimental, self-contained garbage-collection engine for Loxis.  It is
not yet wired into the compiler/VM; it ships as a standalone C++17 library
plus a correctness test suite.  Spec: `../../Todo_AddGC.md`.

## Build & test

```sh
cmake -S src/aarc -B build-aarc -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++
cmake --build build-aarc
ctest --test-dir build-aarc            # or run each test_*.exe directly
```

## What it does

* **Async reference counting.**  Mutator threads never touch the collector's
  bookkeeping.  Every reference change (root add/remove, heap-edge add/remove)
  is pushed as a message onto a per-thread, single-producer/single-consumer,
  lock-free SPSC queue.  A background worker drains the queues and owns the
  refcounts single-threadedly — no atomics on the hot bookkeeping path.
* **Cyclic garbage.**  A second stage runs Tarjan SCC detection over suspects
  whose refcount is held only by their own cycle, computes each component's
  external-incoming-reference count (ECR) and reclaims ECR==0 components.
* **ABA protection.**  Every cell is referenced by a versioned handle
  `{slot, generation}`; the generation bumps on every recycle, so stale
  handles / stale queued messages are detected and dropped, never applied to a
  different object.

## Key design decisions

* **9-byte cells.**  Cells are raw 9-byte payloads in a byte arena with
  accessors (`tag`/`id`/`data`); GC metadata (generation, refcount, outgoing
  edges) lives in side tables so it does not inflate the payload.  A natural
  C++ struct would be 12 bytes — never used.
* **reclaim only at a drain point.**  The worker frees a refcount-zero cell (or
  an ECR-0 component) only after every queue has been drained up to a common
  tail snapshot.  At that instant any reference change that happened-before
  the snapshot is already reflected in the table, so a refcount of zero is
  final.  This is what makes async RC correct across threads without a
  stop-the-world safepoint.
* **edges, not bare counters.**  Roots and heap edges are tracked separately,
  which is what lets the collector tell "internal cycle ref" from "external
  ref" instead of guessing (ECR).

## Status

Phase 0 (allocator / object repr / SPSC / messages), Phase 1 (async ARC +
reclaimer) and Phase 2 (cyclic GC) are implemented and tested.  Phase 3
(scheduler / safepoint) and Phase 4 (optimistic snapshot) are not built as
separate components: the drain-point discipline above already provides their
safety properties (bounded work slices per drain, no STW scanning, stale
state dropped instead of trusted).  A dedicated snapshot/STW layer and
benchmarks vs. a synchronous-ARC baseline remain future work.

## Directory layout

```
core/     cell pool (versioned handles), SPSC queue, GC messages
arc/      ref table, collection engine, runtime + worker + Mutator API
cycle/    Tarjan SCC component finder
tests/    per-phase correctness + concurrency stress tests
```
