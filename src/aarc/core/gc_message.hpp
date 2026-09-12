// AARC core: messages that flow from a mutator thread to the background GC
// worker over a per-thread SPSC queue (Todo_AddGC.md 0.4).
//
// Every reference mutation is expressed as an *edge/root mutation*, so the
// worker can maintain a real object graph (needed later for ECR and cycle
// detection) instead of guessing from bare incref/decref counters.
//
// Message carries full versioned handles.  The worker validates the embedded
// generation against the live pool before applying anything; a message that
// arrives after its target slot was recycled is dropped (ABA protection).
//
// Ordering: the per-queue FIFO is the only ordering guarantee a single mutator
// needs.  Cross-thread ordering is resolved by the worker only reclaiming at
// a drain point where every queue has been consumed up to a common snapshot.
//
// Folding (M10b): within ONE producer batch, root ops on the same handle are
// folded into their net delta (`count`), and a net-zero fold becomes a
// RootCheck -- "no count change, re-evaluate whether this cell is garbage".
// RootCheck is what makes the fold observationally equivalent: the unfolded
// [-1] of an add/remove pair is where the zero/suspect marking happens, so
// dropping both ops without it would silently lose the reclamation trigger.

#pragma once

#include "cell.hpp"
#include <cstdint>

namespace loxis { namespace aarc {

enum class GCOp : uint8_t {
    // The mutator acquired one more *root* reference to `b`
    // (e.g. alloc(), a retained copy of a handle).
    RootAdd,
    // The mutator dropped one root reference to `b`.
    RootRemove,
    // No count change to `b`: re-evaluate its zero/suspect state.  Produced by
    // folding a balanced RootAdd/RootRemove pair on the same handle.
    RootCheck,
    // The mutator created a graph edge `a -> b` (a heap field of `a` now
    // points at `b`).
    EdgeAdd,
    // The mutator removed the graph edge `a -> b`.
    EdgeRemove,
    // Worker-side instruction: stop after draining remaining messages.
    Terminate,
};

struct GCMessage {
    GCOp      op = GCOp::RootAdd;
    ObjHandle a;   // edge source (Edge*), unused for roots
    ObjHandle b;   // root target, or edge target
    // Folded multiplicity for root ops (1 = unfolded).  Ignored by edge ops.
    uint32_t  count = 1;
};

}} // namespace loxis::aarc
