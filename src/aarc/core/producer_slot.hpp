// AARC producer slot: one SPSC queue, its publication sequence and the
// producer-local message batch.
//
// Publication protocol (consistent cut, see runtime.hpp):
//   * pubSeq is a per-producer counter sampled by the worker before (pre) and
//     after (post) it snapshots the queue tails.  A cut is STABLE only when
//     every producer's pubSeq is even and unchanged across the window.
//   * Even/odd is the "publication in flight" bit.  With batching it spans the
//     whole staging window: the producer raises it when the first message is
//     staged and lowers it after the batch has been pushed into the ring.
//     A staged message is a reference the worker cannot see yet, so the cut
//     must stay unstable while any batch is non-empty -- otherwise the worker
//     could reclaim a cell whose RootAdd is still sitting in the batch.
#pragma once

#include "gc_message.hpp"
#include "spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace loxis { namespace aarc {

constexpr size_t kQueueCapacity = 4096;

// Largest producer-local batch.  The effective size is `batchLimit` (a runtime
// knob so the batch-size ablation runs inside one binary).
constexpr uint32_t kMaxBatch = 256;

struct alignas(64) ProducerSlot {
    SpscQueue<GCMessage, kQueueCapacity> queue;
    std::atomic<uint64_t> pubSeq{0};

    // ---- producer-local staging batch -------------------------------------
    // Written ONLY by the owning producer thread; the worker never reads it (it
    // only observes pubSeq).  Kept on its own cacheline so staging does not
    // bounce the queue's head/tail lines.
    alignas(64) GCMessage batch[kMaxBatch];
    // Zero-copy staging: messages are written straight into a reserved run of
    // ring slots, so there is no second copy at flush time.  `spanN` counts the
    // messages written into the currently open (uncommitted) run.
    GCMessage* spanPtr = nullptr;
    uint32_t spanLeft = 0;
    uint32_t spanN = 0;
    uint32_t batchN = 0;      // messages currently staged
    uint32_t batchLimit = 1;  // flush threshold (1 == per-message baseline)
    std::thread::id producerThread;

    // ---- M10b folding scratch (producer thread only) ----------------------
    // Net root delta per handle inside the current batch.  Open-addressed over
    // the packed handle; `foldUsed` lists the touched entries so a flush only
    // resets what it dirtied.
    static constexpr uint64_t kNoFoldKey = ~0ull;   // never a packed handle
    static constexpr uint32_t kFoldSlots = kMaxBatch * 2;
    struct FoldEntry {
        uint64_t key = kNoFoldKey;
        int32_t delta = 0;
        uint32_t outPos = 0;   // where the folded message sits in `folded`
    };
    FoldEntry foldTab[kFoldSlots];
    uint32_t foldUsed[kMaxBatch];
    GCMessage folded[kMaxBatch];

    // Diagnostics (producer-written, relaxed reads once quiesced).
    uint64_t batchesPublished = 0;   // flush() calls that pushed a non-empty batch
    uint64_t msgsPublished = 0;      // messages pushed through flush()
    // Staged / published ops indexed by GCOp value (RootCheck is never folded
    // *in*; it only appears on the out side).
    uint64_t foldIn[5] = {};
    uint64_t foldOut[5] = {};
#if AARC_PROFILE
    // Producer-path attribution (this producer's thread only; AARC_PROFILE).
    uint64_t profStage = 0;   // publish() body without the flush
    uint64_t profFlush = 0;   // flushSlot() body (incl. ring pushes)
    uint64_t profFlushes = 0;
    uint64_t profOps = 0;     // sampled-op counter for the stage timer
    uint64_t profSpin = 0;    // cycles spent waiting for ring room (backpressure)
#endif
};

}} // namespace loxis::aarc
