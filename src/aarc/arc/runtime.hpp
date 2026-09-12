// AARC arc layer: runtime glue.
//
// A Runtime owns one shared CellPool, one AarcEngine and (in async mode) one
// background worker thread plus one SPSC queue per Mutator.  A Mutator is the
// mutator-side handle: each Mutator must only be used from the thread that
// created it (its queue is single-producer).
//
// Three modes:
//   * Sync  — operations call engine.process() directly on the calling thread.
//             Deterministic, for unit tests.  Call collect() to reclaim.
//   * Async — operations push a message onto the calling Mutator's SPSC queue
//             and wake the worker.  The worker drains every queue up to a
//             common tail snapshot, then reclaims.  awaitQuiesce() blocks
//             until one such full round has completed (used by tests to reach
//             a deterministic point).
//   * Adaptive (M2) — the worker engine and per-owner inline LOCAL engines
//             coexist over ONE shared CellPool + RefTable.  LOCAL cells are
//             served synchronously by their owner's own AarcEngine (kind Local);
//             SHARED cells go through the worker exactly as in Async.  Which
//             authority owns a cell is chosen at allocation (allocLocal /
//             allocShared) and every mutation is dispatched by regime.  LOCAL
//             collection happens at the owner's own collectLocal() safe point;
//             awaitQuiesce() still drains/quiesces the SHARED side.
//
// Message batching (M10a): a producer stages up to `batchSize` messages in its
// ProducerSlot and publishes the block with ONE publication window (pubSeq
// odd->even), ONE work-epoch bump and ONE worker wake, instead of paying that
// fixed cost per message.  Per-producer FIFO is untouched (the block is pushed
// in order and the producer is the only writer), the consistent cut is
// untouched (a non-empty batch holds pubSeq odd, so the cut stays unstable),
// and every correctness boundary (batch full, quiesce, close, promotion serve,
// local collect) flushes first.
//
// Liveness contract -- flush before park (M12):
//   A mutator MUST publish its staged batch before it stops making AARC
//   progress: scheduler yield into a blocking wait, I/O block, condition wait,
//   sleep, owner shutdown, or a promotion boundary.  A staged message is a
//   reference the worker cannot see, so while any producer holds one, NO stable
//   cut exists and no thread can complete a sweep or a quiesce.  A live but
//   parked mutator that keeps a batch unpublished would therefore block
//   reclamation indefinitely -- not a memory-safety bug, but a progress bug the
//   execution model must rule out.  The VM/scheduler calls Mutator::willPark()
//   before every potentially-blocking transition; awaitQuiesce() additionally
//   flushes the CALLER's own batches, and a stall diagnostic reports (and in
//   checked builds aborts on) a producer that violates the contract.
//
// Lifecycle rule: all Mutators / producer threads must be finished before the
// Runtime is destroyed; the worker performs a final drain during shutdown.

#pragma once

#include "../core/cell.hpp"
#include "../core/spsc_queue.hpp"
#include "../core/gc_message.hpp"
#include "../core/owner.hpp"
#include "../core/producer_slot.hpp"
#include "../core/root_bag.hpp"
#include "../core/prof.hpp"
#include "engine.hpp"
#include "owner_registry.hpp"
#include "local_components.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#ifdef AARC_TEST_HOOKS
#include <functional>
#endif
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef AARC_ENABLE_STATS
#define AARC_ENABLE_STATS 1
#endif

namespace loxis { namespace aarc {

enum class PromoteOutcome : uint8_t { Pending, Shared, RootReserved, Failed };

// How long a quiesce may wait before the flush-before-park diagnostic fires.
constexpr int64_t kStallWarnMs = 2000;

// Largest run of ring slots reserved for one zero-copy write (a full batch fits
// in one run unless the ring wraps).
constexpr size_t kMaxSpan = kMaxBatch;

inline uint64_t packHandle(ObjHandle h) {
    return (uint64_t(h.index) << 32) | h.gen;
}

class Runtime;
class Mutator {
public:
    // Generic allocation: a cell in the pool's default regime, registered with
    // whatever engine the runtime runs (Sync inline / Async worker).  In
    // Adaptive mode this is the SHARED path — prefer allocLocal()/allocShared().
    ObjHandle alloc();

    // Adaptive (M2): allocate pre-classified into this mutator's LOCAL regime
    // (served inline by `local_`) or the SHARED regime (served by the worker).
    ObjHandle allocLocal();
    ObjHandle allocShared();

    bool rootAdd(ObjHandle h);
    bool rootRemove(ObjHandle h);
    bool edgeAdd(ObjHandle src, ObjHandle dst);
    bool edgeRemove(ObjHandle src, ObjHandle dst);

    bool sync() const { return sync_; }
    bool adaptive() const { return local_ != nullptr; }

#if AARC_PROFILE
    // Producer-path attribution counters (this mutator's thread only).
    struct Prof {
        uint64_t poolAlloc = 0;   // CellPool::allocOwned
        uint64_t rootBag = 0;     // RootBag add / removeOne
        uint64_t ops = 0;         // total ops (diagnostic)
        uint64_t poolOps = 0;     // per-phase sample counters: sharing one
        uint64_t bagOps = 0;      // counter across phases makes the 1/64
                                  // sample land on the same phase every time
        uint64_t prologue = 0;    // handle-validity checks (root/edge op entry)
        uint64_t preOps = 0;
    };
    const Prof& prof() const { return prof_; }
    const RootBag& roots() const { return roots_; }
#endif
    OwnerToken ownerToken() const { return owner_; }

    // Adaptive: this mutator's inline LOCAL engine (null in Sync/Async).
    AarcEngine* localEngine() const { return local_.get(); }

    // Adaptive: local safe point — reclaim this owner's LOCAL zero/cyclic
    // garbage inline.  Independent of the worker's SHARED rounds.
    void collectLocal();

    // Adaptive (M3): serve any pending promotion requests aimed at this owner's
    // LOCAL cells.  An escaping thread that wants one of our cells blocks until
    // we run this at a local safe point (it is also polled automatically at
    // every adaptive op entry).  Returns the number of cells handed to the
    // worker.  Owners must keep polling while any of their cells may be shared;
    // a vanished owner deadlocks its requesters (design doc §9 risk 1).
    uint64_t pollPromotions();

    // Adaptive: count of ops that could not be completed because the authority
    // boundary could not be crossed — in M3 every well-formed escape is handled
    // by a promotion, so this should stay 0 (it fires only if a promotion
    // cannot be arranged, e.g. the owning thread has vanished).
    uint64_t promotionRequired() const { return promotionRequired_; }

    // End this producer/owner lifetime.  The operation is idempotent and must
    // run on the mutator's owner thread before that thread exits.
    void close();

    // Publish everything currently staged in this mutator's batch.  Called by
    // the owner thread only.  Correctness boundaries (a blocking wait, a
    // promotion serve, a quiesce, owner close) must flush first so no staged
    // message is invisible to the worker while this thread is parked.
    void flush();

    // Flush-before-park (M12): call before entering ANY state in which this
    // thread may stop making AARC progress (scheduler yield into a blocking
    // wait, I/O, condition wait, sleep, owner shutdown).  While a staged batch
    // exists no stable cut exists, so a parked mutator holding one would block
    // every other thread's sweep/quiesce.  Idempotent and cheap when empty.
    void willPark();

    ~Mutator() { close(); }

private:
    friend class Runtime;
    // Where does a handle live?  Decides which engine (if any) may write it.
    //   MyLocal       -> regime LOCAL, owner == this mutator  (inline)
    //   SharedWorker  -> regime SHARED                         (worker)
    //   Foreign       -> a LOCAL cell owned by another thread, or PROMOTING:
    //                    frozen during a hand-off; either way wait/promote (M3)
    enum class Authority { MyLocal, SharedWorker, Foreign };
    Authority classify(ObjHandle h) const;

    Mutator(Runtime& rt, ProducerSlot* slot, bool syncMode,
            OwnerToken owner, std::unique_ptr<AarcEngine> local = {})
        : rt_(rt), slot_(slot), sync_(syncMode), owner_(owner),
          local_(std::move(local)) {}

    // Owner-side safepoint: drain pending promotion requests for OUR cells
    // before we touch them (cheap: one atomic load when nothing is pending).
    void maybeServe();
    // Root-op / edge-op dispatch with promotion-on-escape (M3).
    bool adaptiveRootOp(GCOp op, ObjHandle h);
    bool adaptiveEdgeOp(GCOp op, ObjHandle src, ObjHandle dst);
    // Block until `h` is SHARED.  Foreign RootAdd reserves its root in the
    // owner engine before hand-off, closing the owner-exit liveness gap.
    PromoteOutcome promoteHandle(ObjHandle h, bool reserveRoot = false);
    void submit(const GCMessage& m);

    Runtime& rt_;
    ProducerSlot* slot_;
    bool sync_;
    OwnerToken owner_; // recycle-pile affinity and LOCAL authority
    std::unique_ptr<AarcEngine> local_;   // adaptive: inline LOCAL engine
    bool closed_ = false;
    RootBag roots_;
    uint64_t promotionRequired_ = 0;
#if AARC_PROFILE
    Prof prof_;
#endif
};

class Runtime {
public:
    enum class Mode { Sync, Async, Adaptive };

#ifdef AARC_TEST_HOOKS
    struct TestHooks {
        std::function<void(size_t)> afterProducerPre;
        std::function<void()> afterQueuePush;
        std::function<void()> afterQuiesceCapture;
        std::function<void()> afterRound;
        std::function<void()> workerExited;
        std::function<void()> shutdownSetStop;
    };
#endif

    explicit Runtime(uint32_t poolCapacity, Mode mode = Mode::Async,
                     bool spawnWorker = true, uint32_t batchSize = 1,
                     bool fold = false, bool spanPath = true)
        : pool_(poolCapacity, mode == Mode::Sync ? Regime::Local
                                                 : Regime::Shared),
          refs_(pool_.capacity()),
          engine_(pool_, refs_,
                  mode == Mode::Sync ? Regime::Local : Regime::Shared,
                  kNoOwnerToken),
          mode_(mode),
          batchSize_(batchSize ? batchSize : 1),
          fold_(fold),
          spanPath_(spanPath),
          promote_(CellPool::kMaxOwners),
          promoteFlag_(CellPool::kMaxOwners)
#ifdef AARC_CONSERVATIVE
          , comps_(poolCapacity)
#endif
          {
        if (batchSize_ > kMaxBatch) batchSize_ = kMaxBatch;
        // std::atomic is not copy-constructible, so zero the poll bits by hand.
        for (auto& f : promoteFlag_)
            f.store(0, std::memory_order_relaxed);
        // `spawnWorker` exists for DIAGNOSTICS (isolate idle-worker cost on pure
        // thread-local workloads).  In Adaptive mode a pure-LOCAL run never
        // submits to the worker, so omitting the thread must not change results
        // if the idle thread itself is not the tax.
        if ((mode == Mode::Async || mode == Mode::Adaptive) && spawnWorker)
            worker_ = std::thread([this] { workerLoop(); });
        if (mode == Mode::Adaptive)
            engine_.setMixed(true);   // see AarcEngine::setMixed
    }

    ~Runtime() { shutdown(); }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    CellPool& pool() { return pool_; }
    AarcEngine& engine() { return engine_; }
    Mode mode() const { return mode_; }
    bool adaptive() const { return mode_ == Mode::Adaptive; }
    uint32_t liveCells() const { return pool_.used(); }

#ifdef AARC_TEST_HOOKS
    void setTestHooks(TestHooks hooks) { testHooks_ = std::move(hooks); }
#endif

    // M3 diagnostics: is a promotion request pending for `owner` (their cheap
    // safepoint poll bit)?  This is on every adaptive op's hot path, so it must
    // NOT consult the (mutex-guarded) owner registry: a live mutator is Active
    // by construction and close() clears the flag on the way out.
    bool promotePending(OwnerToken owner) const {
        return owner.id < CellPool::kMaxOwners &&
               promoteFlag_[owner.id].load(std::memory_order_acquire) != 0;
    }
    uint64_t promotionsDone() const {
        return promotionsDone_.load(std::memory_order_relaxed);
    }

    // Batch-transport diagnostics (producer-side counters; relaxed reads).
    uint64_t batchesPublished() const {
        return batches_.load(std::memory_order_relaxed);
    }
    uint64_t msgsPublished() const {
        return msgsPublished_.load(std::memory_order_relaxed);
    }
    uint64_t wakeups() const { return wakeups_.load(std::memory_order_relaxed); }
    uint32_t batchSize() const { return batchSize_; }
    bool folding() const { return fold_; }
    // M11a: write messages straight into a reserved ring run (zero copy) vs the
    // pre-M11a "stage in batch[], push per message" path.  A/B switch.
    void setSpanPath(bool v) { spanPath_ = v; }
    bool spanPath() const { return spanPath_; }
    // M11b: producer-local slot cache on/off (A/B switch).
    void setLocalCache(bool v) { pool_.setLocalCache(v); }
    // M12b: RootBag-implication fast path for rootRemove (A/B switch).
    void setFastRootValid(bool v) { fastRootValid_ = v; }
    bool fastRootValid() const { return fastRootValid_; }

    // M10b folding stats: staged vs published root/edge ops.  Summed over the
    // per-producer slots (producer-local counters, so no shared-line traffic).
    struct FoldStats {
        // Indexed by GCOp: 0 RootAdd, 1 RootRemove, 3 EdgeAdd, 4 EdgeRemove.
        uint64_t in[5] = {};
        uint64_t out[5] = {};
        uint64_t inTotal() const { return in[0] + in[1] + in[3] + in[4]; }
        // Outputs include RootCheck (index 2): a folded balanced pair still
        // costs one message, so it must count toward the emitted total.
        uint64_t outTotal() const {
            return out[0] + out[1] + out[2] + out[3] + out[4];
        }
        double ratio() const {
            return outTotal() ? double(inTotal()) / double(outTotal()) : 0.0;
        }
    };
#if AARC_PROFILE
    // Producer-local attribution counters (summed over slots; cold path).
    struct SlotProf {
        uint64_t stage = 0, flush = 0, spin = 0, flushes = 0, stageOps = 0;
    };
    SlotProf slotProf() {
        SlotProf s;
        std::lock_guard<std::mutex> lk(slotsMu_);
        for (const auto& slot : slots_) {
            s.stage += slot->profStage;
            s.flush += slot->profFlush;
            s.spin += slot->profSpin;
            s.flushes += slot->profFlushes;
            s.stageOps += slot->profOps;
        }
        return s;
    }
#endif

    FoldStats foldStats() {
        FoldStats s;
        std::lock_guard<std::mutex> lk(slotsMu_);
        for (const auto& slot : slots_)
            for (int i = 0; i < 5; ++i) {
                s.in[i] += slot->foldIn[i];
                s.out[i] += slot->foldOut[i];
            }
        return s;
    }
    // Average staged messages per published batch (0 when nothing was sent).
    double avgBatch() const {
        const uint64_t b = batchesPublished();
        return b ? double(msgsPublished()) / double(b) : 0.0;
    }

    // Backpressure / worker diagnostics.
#if AARC_ENABLE_STATS
    uint64_t spinCount() const { return spins_.load(std::memory_order_relaxed); }
    uint64_t roundsDone() const { return rounds_; }
    size_t peakQueueOccupancy() const { return peakOcc_; }
    uint64_t invalidCuts() const { return invalidCuts_; }
    uint64_t quiesceStalls() const { return quiesceStalls_; }
#else
    uint64_t spinCount() const { return 0; }
    uint64_t roundsDone() const { return 0; }
    size_t peakQueueOccupancy() const { return 0; }
    uint64_t invalidCuts() const { return 0; }
    uint64_t quiesceStalls() const { return quiesceStalls_; }
#endif

    // Create a mutator session bound to the calling thread.
    std::unique_ptr<Mutator> makeMutator() {
        std::unique_ptr<Mutator> m;
        if (mode_ == Mode::Sync) {
            m.reset(new Mutator(*this, nullptr, true, kNoOwnerToken));
            return m;
        }
        // Async & Adaptive share the queue + owner machinery; Adaptive adds a
        // per-owner inline LOCAL engine.  Owner ids are leased, never reused
        // while their previous lifetime can still be observed.
        OwnerToken owner;
        if (!ownerRegistry_.acquire(owner)) return nullptr;
        auto slot = std::make_unique<ProducerSlot>();
        ProducerSlot* raw = slot.get();
        raw->batchLimit = batchSize_;
        raw->producerThread = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> lk(slotsMu_);
            const size_t idx = slots_.size();
            slots_.push_back(std::move(slot));
            if (idx < CellPool::kMaxOwners) {
                slotIndex_[idx] = raw;
                slotCount_.store(idx + 1, std::memory_order_release);
            }
            slotsEpoch_.fetch_add(1, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> lk(promote_[owner.id].mu);
            promote_[owner.id].token = owner;
            promote_[owner.id].accepting = true;
        }
        if (mode_ == Mode::Adaptive) {
            auto local = std::make_unique<AarcEngine>(pool_, refs_, Regime::Local, owner);
            local->setMixed(true);
#ifdef AARC_CONSERVATIVE
            local->setComponents(&comps_);
#endif
            m.reset(new Mutator(*this, raw, false, owner, std::move(local)));
        } else {
            m.reset(new Mutator(*this, raw, false, owner));
        }
        return m;
    }

    // Sync mode: reclaim zero-count cells now.
    // Async/Adaptive mode: block until the worker has finished one full
    // drain+sweep round (every SHARED-side message enqueued before this call is
    // applied).  Adaptive LOCAL cells are reclaimed by the owning mutator's
    // collectLocal(), not here.
    void awaitQuiesce() {
        if (mode_ == Mode::Sync) {
            engine_.sweep();
            return;
        }
        // Any batch staged by THIS thread must be published first: a staged
        // message keeps the cut unstable, so leaving one behind would make our
        // own quiesce wait (and every other caller's) hang.
        flushOwnBatches();
        const uint64_t ticket =
            quiesceIssued_.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            // notify under the mutex so it cannot land in the worker's
            // release-and-register window (lost wakeup).
            std::lock_guard<std::mutex> lk(cvMu_);
            cvWork_.notify_one();
        }
        std::unique_lock<std::mutex> lk(cvMu_);
        auto done = [this, ticket] {
            return quiesceCompleted_.load(std::memory_order_acquire) >= ticket;
        };
        if (!cvDone_.wait_for(lk, std::chrono::milliseconds(kStallWarnMs), done)) {
            reportStalledProducers();   // diagnostic only; keep waiting
            cvDone_.wait(lk, done);
        }
    }

private:
    friend class Mutator;

    void wakeWorker() {
        wakeups_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(cvMu_);
        cvWork_.notify_one();
    }

    // ---- M3: ownership-transfer promotion ---------------------------------
    // One gate per owner id.  Escaping threads (requester U, owner != target's
    // owner) register their target LOCAL cell(s) here and block until the owner
    // (T) serves them at a local safe point.  `epoch` advances on every
    // completed hand-off so a waiter can tell "my registration round was
    // served" from "the owner is busy with someone else's".
    struct PromoteRequest {
        ObjHandle root;
        bool reserveRoot = false;
        bool rootReserved = false;
        PromoteOutcome outcome = PromoteOutcome::Pending;
    };

    struct PromoteGate {
        std::mutex mu;
        std::condition_variable cv;
        OwnerToken token = kNoOwnerToken;
        bool accepting = false;
        bool requested = false;
        std::vector<std::shared_ptr<PromoteRequest>> requests;
        uint64_t epoch = 0;
    };

    // The owner safepoint polls this bit on every adaptive op.  It is packed by
    // default (adjacent owners in one cacheline) which false-shares the poll
    // across producer threads and throttles multi-thread LOCAL throughput; pad
    // each owner's bit onto its own cacheline so per-thread polls stay private.
    struct PromoteFlag {
        alignas(64) std::atomic<uint8_t> v{0};
        uint8_t load(std::memory_order mo) const noexcept { return v.load(mo); }
        void store(uint8_t x, std::memory_order mo) noexcept { v.store(x, mo); }
    };

    // Stall diagnostic (M12): a quiesce that waits longer than kStallWarnMs is
    // almost certainly blocked by a live producer holding an unpublished batch.
    // Reports the culprits on stderr, counts the event, and (checked builds)
    // aborts -- the flush-before-park contract was violated.
    void reportStalledProducers();

    // Owner-side: consume pending requests and hand each requested component to
    // the worker.  `local` is the owner's inline engine (its scratch is settled
    // first).  Returns the number of cells promoted.  Owner thread only.
    size_t servePromotions(OwnerToken owner, AarcEngine& local);

    // The actual hand-off for a set of roots, run on the owner thread: settle
    // the owner's LOCAL scratch, compute the undirected component closure of the
    // roots, freeze (PROMOTING) then commit (SHARED), bump the gate epoch and
    // wake waiters.  Returns cells promoted.
    size_t promoteLocalComponent(OwnerToken owner, AarcEngine& local,
                                 const std::vector<ObjHandle>& roots);

    // Requester-side: register `h` (a LOCAL cell owned by someone else) with its
    // owner's gate and block until it is SHARED (or dies).  Returns false if the
    // handle dies or the owner is unreachable.
    PromoteOutcome promoteForeign(ObjHandle h, OwnerToken callerOwner,
                                  bool reserveRoot);

    // Wait (on the owner's gate) until `h` is no longer frozen PROMOTING — i.e.
    // the in-flight hand-off on its owner committed.
    void waitUntilNotPromoting(ObjHandle h);

    // Undirected connected closure of `seeds` within the LIVE cells owned by
    // `owner`, following edges both forward and reverse.  Caller is the owner
    // thread at a safe point (no concurrent writer of these rows).
    std::vector<uint32_t> closureLocal(OwnerToken owner,
                                       const std::vector<ObjHandle>& seeds);
    bool closeOwner(OwnerToken owner, AarcEngine* local, ProducerSlot* slot,
                    const RootBag& roots);

    // Stage one message into `slot`'s producer-local batch and publish the block
    // when it is full (see the batching note in the header comment).
    void publish(ProducerSlot* slot, const GCMessage& message);

    // Push a staged batch into the SPSC ring and close its publication window.
    // Producer thread only.
    void flushSlot(ProducerSlot* slot);

    // Zero-copy staging: append one message to the ring's reserved run, taking
    // a new run (and spinning) when the current one is exhausted.  Producer only.
    void writeToSpan(ProducerSlot* slot, const GCMessage& m);

    // Publish the messages written into the open run so far.
    void commitStaged(ProducerSlot* slot);

    // M10b: fold one staged batch into `slot->folded` and return its length.
    // Root ops on the same handle+generation collapse to their net delta; a net
    // zero collapses to RootCheck (see gc_message.hpp).  Only ops from THIS
    // batch take part -- a barrier (quiesce / close / promotion / local collect)
    // always flushes first, so a batch never spans one.
    uint32_t foldBatch(ProducerSlot* slot);

    // Publish every batch staged by the calling thread (quiesce / shutdown
    // boundary).  Slots are per-mutator and only ever appended, so the raw
    // pointers stay valid; they are collected under the slot lock and flushed
    // after it is released.
    void flushOwnBatches();

    // Drain every queue up to a common tail snapshot, then reclaim.
    //
    // `isSyncRound`: true when this round started *after* an awaitQuiesce()
    // request.  Only such a round may satisfy the request — a round that was
    // already running may have taken its tail snapshots before the last few
    // messages were pushed, so it cannot vouch that the graph is settled.
    bool doRound(uint64_t serveThrough) {
        const uint64_t slotsBefore = slotsEpoch_.load(std::memory_order_acquire);
        if (slotsBefore != roundSlotsEpoch_) {
            std::lock_guard<std::mutex> lk(slotsMu_);
            roundSlots_.clear();
            roundSlots_.reserve(slots_.size());
            for (auto& slot : slots_) roundSlots_.push_back(slot.get());
            roundSlotsEpoch_ = slotsEpoch_.load(std::memory_order_acquire);
        }
        const size_t n = roundSlots_.size();
        roundTails_.resize(n);
        roundPre_.resize(n);
        roundPost_.resize(n);
#if AARC_ENABLE_STATS
        ++rounds_;
        size_t occ = 0;
        for (auto* slot : roundSlots_) occ += slot->queue.size();
        if (occ > peakOcc_) peakOcc_ = occ;
#endif

        bool stable = true;
        for (size_t i = 0; i < n; ++i) {
            roundPre_[i] = roundSlots_[i]->pubSeq.load(std::memory_order_seq_cst);
#ifdef AARC_TEST_HOOKS
            if (testHooks_.afterProducerPre) testHooks_.afterProducerPre(i);
#endif
            if (roundPre_[i] & 1u) stable = false;
        }
        for (size_t i = 0; i < n; ++i)
            roundTails_[i] = roundSlots_[i]->queue.tailSnapshot();
        for (size_t i = 0; i < n; ++i) {
            roundPost_[i] = roundSlots_[i]->pubSeq.load(std::memory_order_seq_cst);
            if ((roundPost_[i] & 1u) || roundPost_[i] != roundPre_[i]) stable = false;
        }
        if (slotsEpoch_.load(std::memory_order_acquire) != slotsBefore)
            stable = false;
#if AARC_ENABLE_STATS
        if (!stable) ++invalidCuts_;
#endif

        for (size_t i = 0; i < n; ++i) {
            ProducerSlot* slot = roundSlots_[i];
            const size_t tail = roundTails_[i];
            GCMessage m;
            while (slot->queue.headPosition() < tail) {
                if (!slot->queue.tryPop(m)) break;
                engine_.process(m);
            }
        }
        if (stable) {
            engine_.sweep();
            uint64_t completed = quiesceCompleted_.load(std::memory_order_relaxed);
            while (completed < serveThrough &&
                   !quiesceCompleted_.compare_exchange_weak(
                       completed, serveThrough, std::memory_order_release,
                       std::memory_order_relaxed)) {}
            {
                // notify under the mutex to avoid the lost-wakeup window.
                std::lock_guard<std::mutex> lk(cvMu_);
                cvDone_.notify_all();
            }
        }
#ifdef AARC_TEST_HOOKS
        if (testHooks_.afterRound) testHooks_.afterRound();
#endif
        return stable;
    }

    void workerLoop() {
        // Nothing has been drained yet, so the drained-through epoch starts at
        // zero.  Seeding this from the CURRENT epoch would swallow a wake for a
        // batch published before this thread reached its first wait (the
        // publisher's notify lands before the wait is registered): the message
        // would sit in the ring until some later publish or quiesce.
        uint64_t seenWork = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(cvMu_);
                cvWork_.wait(lk, [this, seenWork] {
                    return stop_.load(std::memory_order_acquire) ||
                           workEpoch_.load(std::memory_order_acquire) != seenWork ||
                           quiesceIssued_.load(std::memory_order_acquire) >
                               quiesceCompleted_.load(std::memory_order_acquire);
                });
            }
            const uint64_t roundWork = workEpoch_.load(std::memory_order_acquire);
            const uint64_t serveThrough =
                quiesceIssued_.load(std::memory_order_acquire);
#ifdef AARC_TEST_HOOKS
            if (testHooks_.afterQuiesceCapture) testHooks_.afterQuiesceCapture();
#endif
            const bool stable = doRound(serveThrough);
            seenWork = roundWork;
            if (!stable) continue;
            if (stop_.load(std::memory_order_acquire) &&
                workEpoch_.load(std::memory_order_acquire) == seenWork &&
                quiesceCompleted_.load(std::memory_order_acquire) >=
                    quiesceIssued_.load(std::memory_order_acquire)) {
#ifdef AARC_TEST_HOOKS
                if (testHooks_.workerExited) testHooks_.workerExited();
#endif
                return;
            }
        }
    }

    void shutdown() {
        if ((mode_ == Mode::Async || mode_ == Mode::Adaptive) && worker_.joinable()) {
            {
                std::lock_guard<std::mutex> lk(cvMu_);
                stop_.store(true, std::memory_order_release);
#ifdef AARC_TEST_HOOKS
                if (testHooks_.shutdownSetStop) testHooks_.shutdownSetStop();
#endif
                cvWork_.notify_all();
            }
            worker_.join();
            // Move any cells left in per-owner recycle piles back to the
            // shared free list now that no producer will pop them again.
            pool_.drainRecycleAll();
            // Final drain above already freed everything marked zero.
            // But some messages may still sit in queues if the mutator threads
            // were not all joined — see lifecycle rule in the header comment.
        }
    }

    CellPool pool_;
    RefTable refs_;        // shared by the inline (Local) and worker (Shared) engines
    AarcEngine engine_;
    Mode mode_;
    uint32_t batchSize_ = 1;                 // producer batch threshold (M10a)
    bool fold_ = false;                      // intra-batch folding (M10b)
    bool spanPath_ = true;                   // zero-copy ring span (M11a)
    bool fastRootValid_ = false;             // M12b rootRemove fast path
    std::vector<PromoteGate> promote_;       // per-owner promotion gates (M3)
    std::vector<PromoteFlag> promoteFlag_;   // cheap owner poll bits (cacheline-padded)
    OwnerRegistry ownerRegistry_;
#ifdef AARC_CONSERVATIVE
    ConservativeComponents comps_;   // conservative LOCAL may-components
#endif
    std::atomic<uint64_t> promotionsDone_{0};
    std::atomic<uint64_t> batches_{0};         // published batches (M10a stats)
    std::atomic<uint64_t> msgsPublished_{0};   // messages in published batches
    std::atomic<uint64_t> wakeups_{0};

    std::mutex slotsMu_;
    std::vector<std::unique_ptr<ProducerSlot>> slots_;
    std::atomic<uint64_t> slotsEpoch_{0};
    // Lock-free snapshot for diagnostics: entries [0, slotCount_) are published
    // slot pointers (append-only, stable).  Lets the stall reporter run while
    // holding cvMu_ without taking slotsMu_ (the reverse lock order exists on
    // the flush path).
    std::atomic<size_t> slotCount_{0};
    ProducerSlot* slotIndex_[CellPool::kMaxOwners] = {};

    // Worker-round scratch (worker thread only).  Reused across rounds so a
    // round costs no heap traffic; the slot snapshot is rebuilt only when a
    // producer joins (slots are appended, never removed, so pointers are stable).
    std::vector<ProducerSlot*> roundSlots_;
    uint64_t roundSlotsEpoch_ = 0;
    std::vector<size_t> roundTails_;
    std::vector<uint64_t> roundPre_;
    std::vector<uint64_t> roundPost_;

    std::thread worker_;
    std::mutex cvMu_;
    std::condition_variable cvWork_;
    std::condition_variable cvDone_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> workEpoch_{0};
    std::atomic<uint64_t> quiesceIssued_{0};
    std::atomic<uint64_t> quiesceCompleted_{0};
#ifdef AARC_TEST_HOOKS
    TestHooks testHooks_;
#endif
#if AARC_ENABLE_STATS
    std::atomic<uint64_t> spins_{0};
    uint64_t rounds_ = 0;
    size_t peakOcc_ = 0;
    uint64_t invalidCuts_ = 0;
    uint64_t quiesceStalls_ = 0;
#endif
};

// ---- Runtime: M3 promotion coordinator bodies ----------------------------

inline void Runtime::reportStalledProducers() {
#if AARC_ENABLE_STATS
    ++quiesceStalls_;
#endif
    std::fprintf(stderr,
                 "[aarc] quiesce stalled >%lldms -- flush-before-park contract "
                 "violated: a live producer is holding an unpublished batch, so "
                 "no stable cut exists\n",
                 static_cast<long long>(kStallWarnMs));
    const size_t n = slotCount_.load(std::memory_order_acquire);
    for (size_t i = 0; i < n && i < CellPool::kMaxOwners; ++i) {
        const ProducerSlot* slot = slotIndex_[i];
        if (!slot) continue;
        const uint64_t seq = slot->pubSeq.load(std::memory_order_acquire);
        if (seq & 1u)
            std::fprintf(stderr,
                         "[aarc]   stalled producer #%zu: pubSeq=%llu staged=%u "
                         "limit=%u (call Mutator::willPark() before blocking)\n",
                         i, static_cast<unsigned long long>(seq), slot->batchN,
                         slot->batchLimit);
    }
#if AARC_CHECK_INVARIANTS
    assert(false && "flush-before-park violated: mutator parked with a staged batch");
#endif
}

inline size_t Runtime::servePromotions(OwnerToken owner, AarcEngine& local) {
    if (owner.id >= CellPool::kMaxOwners ||
        ownerRegistry_.state(owner) == OwnerState::Free) return 0;
    PromoteGate& g = promote_[owner.id];
    std::vector<std::shared_ptr<PromoteRequest>> requests;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (!g.requested) {
            promoteFlag_[owner.id].store(0, std::memory_order_relaxed);
            return 0;
        }
        g.requested = false;
        requests.swap(g.requests);
    }

    std::vector<ObjHandle> roots;
    roots.reserve(requests.size());
    for (const auto& request : requests) {
        if (!pool_.valid(request->root) ||
            pool_.regimeOf(request->root) != Regime::Local ||
            pool_.ownerTokenAt(request->root.index) != owner) {
            continue;
        }
        if (request->reserveRoot) {
            local.process({GCOp::RootAdd, kNullHandle, request->root});
            request->rootReserved = true;
        }
        roots.push_back(request->root);
    }
    const size_t promoted = promoteLocalComponent(owner, local, roots);

    {
        std::lock_guard<std::mutex> lk(g.mu);
        for (const auto& request : requests) {
            if (!pool_.valid(request->root))
                request->outcome = PromoteOutcome::Failed;
            else if (pool_.regimeOf(request->root) == Regime::Shared)
                request->outcome = request->rootReserved
                                       ? PromoteOutcome::RootReserved
                                       : PromoteOutcome::Shared;
            else
                request->outcome = PromoteOutcome::Failed;
        }
        ++g.epoch;
        promoteFlag_[owner.id].store(g.requested ? 1 : 0, std::memory_order_relaxed);
    }
    g.cv.notify_all();
    return promoted;
}

inline size_t Runtime::promoteLocalComponent(OwnerToken owner, AarcEngine& local,
                                             const std::vector<ObjHandle>& roots) {
    if (owner.id >= CellPool::kMaxOwners || roots.empty() ||
        ownerRegistry_.state(owner) == OwnerState::Free) return 0;
    // Keep only seeds that are still this owner's live LOCAL cells.
    std::vector<ObjHandle> seeds;
    for (const ObjHandle& h : roots)
        if (pool_.valid(h) && pool_.regimeOf(h) == Regime::Local &&
            pool_.ownerTokenAt(h.index) == owner)
            seeds.push_back(h);
    if (seeds.empty()) return 0;

    // Settle this owner's LOCAL scratch BEFORE freezing: once a cell is handed
    // over the owner's engine must not still hold zero/suspect entries for it
    // (its own sweep would then reclaim a cell it no longer owns).  The sweep
    // also reclaims garbage the closure would otherwise drag into the component.
    // Owner thread only — it is the single writer of its LOCAL cells.
    local.sweep();

    // The sweep may have reclaimed garbage seeds; keep the survivors.
    std::vector<ObjHandle> live;
    for (const ObjHandle& h : seeds)
        if (pool_.valid(h) && pool_.regimeOf(h) == Regime::Local &&
            pool_.ownerTokenAt(h.index) == owner)
            live.push_back(h);
    if (live.empty()) return 0;

    std::vector<uint32_t> promoteList;
#ifdef AARC_CONSERVATIVE
    // Conservative scheme: promote every live member of each seed's may-
    // component.  A component never splits, so this can only over-promote.
    for (const ObjHandle& s : live) {
        comps_.promoteComponent(owner, s.index, [&](uint32_t m) {
            promoteList.push_back(m);
        });
    }
    if (promoteList.empty()) return 0;
#else
    const std::vector<uint32_t> comp = closureLocal(owner, live);
    if (comp.empty()) return 0;
    promoteList = comp;
#endif

    // Two-phase hand-off: freeze then commit.  No third authority can observe a
    // member mid-flight — LOCAL cells are single-writer to their owner, the
    // worker never sees a message for them before commit, and escaping threads
    // re-check through the owner's epoch — so the commit is the single
    // cut-over point.  The worker needs no explicit adoption step: the RefTable
    // rows are already the shared truth; promotion transfers *ownership*, never
    // data (design doc §6.2.1).  The freeze (PROMOTING) still guards the window
    // for the invariant machinery and any future concurrent escape.
    for (uint32_t i : promoteList)
        pool_.setRegime(ObjHandle{i, pool_.generationOf(i)}, Regime::Promoting);
    for (uint32_t i : promoteList)
        pool_.setRegime(ObjHandle{i, pool_.generationOf(i)}, Regime::Shared);
    local.forgetPromoted(promoteList);

    {
        std::lock_guard<std::mutex> lk(promote_[owner.id].mu);
        ++promote_[owner.id].epoch;      // release: makes every flip above visible
    }
    promote_[owner.id].cv.notify_all();
    promotionsDone_.fetch_add(1, std::memory_order_relaxed);
    return promoteList.size();
}

#ifndef AARC_CONSERVATIVE
inline std::vector<uint32_t> Runtime::closureLocal(
    OwnerToken owner, const std::vector<ObjHandle>& seeds) {
    auto isMine = [&](uint32_t idx) {
        return pool_.isLiveAt(idx) && pool_.regimeAt(idx) == Regime::Local &&
               pool_.ownerTokenAt(idx) == owner;
    };

    // The persistent RefTable reverse links make this an undirected walk over
    // the LOCAL component itself.  There is intentionally no pool scan here:
    // work is proportional to the component vertices plus their incident edges.
    // AARC_LEGACY_POOL_CLOSURE restores the old O(pool) reverse-adjacency build
    // for the ablation measurement only.
#ifdef AARC_LEGACY_POOL_CLOSURE
    const uint32_t cap = pool_.capacity();
    std::vector<uint8_t> seen(cap, 0);
    std::vector<uint32_t> comp;
    std::vector<uint32_t> stack;
    auto markAndPush = [&](uint32_t idx, bool mine) {
        if (mine && !seen[idx]) { seen[idx] = 1; stack.push_back(idx); }
    };
    std::unordered_map<uint32_t, std::vector<uint32_t>> rev;
    pool_.forEachLive([&](ObjHandle h) {
        if (!isMine(h.index)) return;
        for (const ObjHandle& t : refs_.outs(h.index))
            if (pool_.valid(t) && isMine(t.index)) rev[t.index].push_back(h.index);
    });
    for (const ObjHandle& s : seeds) markAndPush(s.index, true);
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        comp.push_back(i);
        for (const ObjHandle& t : refs_.outs(i)) markAndPush(t.index, isMine(t.index));
        auto it = rev.find(i);
        if (it != rev.end())
            for (uint32_t j : it->second) markAndPush(j, isMine(j));
    }
    return comp;
#else
    std::unordered_set<uint32_t> seen;
    seen.reserve(seeds.size() * 2 + 1);
    std::vector<uint32_t> comp;
    std::vector<uint32_t> stack;
    for (const ObjHandle& s : seeds) {
        if (isMine(s.index) && seen.insert(s.index).second)
            stack.push_back(s.index);
    }
    while (!stack.empty()) {
        const uint32_t i = stack.back();
        stack.pop_back();
        comp.push_back(i);

        for (const ObjHandle& t : refs_.outs(i)) {
            if (pool_.valid(t) && isMine(t.index) && seen.insert(t.index).second)
                stack.push_back(t.index);
        }
        for (const ObjHandle& source : refs_.ins(i)) {
            if (pool_.valid(source) && isMine(source.index) &&
                seen.insert(source.index).second)
                stack.push_back(source.index);
        }
    }
    return comp;
#endif
}
#endif


inline bool Runtime::closeOwner(
    OwnerToken owner, AarcEngine* local, ProducerSlot* slot,
    const RootBag& roots) {
    if (owner.id >= CellPool::kMaxOwners || !ownerRegistry_.beginClose(owner))
        return false;

    PromoteGate& g = promote_[owner.id];
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.token != owner) return false;
        g.accepting = false;
    }

    if (local) {
        servePromotions(owner, *local);
        // Hand every remaining LOCAL cell to the worker in ONE pass.  closureLocal
        // walks the undirected component from each seed, so passing the whole
        // live set as seeds promotes every disconnected component together.
        // (The old one-seed-per-iteration loop rescanned the live set each time
        // and was O(live^2) when an owner held many disconnected LOCAL cells.)
        std::vector<ObjHandle> seeds;
        pool_.forEachLive([&](ObjHandle h) {
            if (pool_.regimeOf(h) == Regime::Local &&
                pool_.ownerTokenAt(h.index) == owner)
                seeds.push_back(h);
        });
        if (!seeds.empty())
            promoteLocalComponent(owner, *local, seeds);
    }

    bool ownerLocalRemains = false;
    pool_.forEachLive([&](ObjHandle h) {
        if (pool_.ownerTokenAt(h.index) == owner &&
            pool_.regimeOf(h) != Regime::Shared)
            ownerLocalRemains = true;
    });
    {
        std::lock_guard<std::mutex> lk(g.mu);
        ++g.epoch;
        g.requested = false;
        for (const auto& request : g.requests)
            request->outcome = PromoteOutcome::Failed;
        g.requests.clear();
    }
    promoteFlag_[owner.id].store(0, std::memory_order_release);
    g.cv.notify_all();
    if (ownerLocalRemains) return false;

    roots.each([&](uint64_t key, uint32_t count) {
        for (uint32_t c = 0; c < count; ++c)
            publish(slot, {GCOp::RootRemove, kNullHandle,
                           ObjHandle{uint32_t(key >> 32), uint32_t(key)}});
    });
    awaitQuiesce();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.token = kNoOwnerToken;
    }
    return ownerRegistry_.release(owner);
}

inline PromoteOutcome Runtime::promoteForeign(
    ObjHandle h, OwnerToken callerOwner, bool reserveRoot) {
    if (!pool_.valid(h)) return PromoteOutcome::Failed;
    const OwnerToken own = pool_.ownerTokenAt(h.index);
    if (own.id >= CellPool::kMaxOwners || own == callerOwner ||
        ownerRegistry_.state(own) == OwnerState::Free)
        return PromoteOutcome::Failed;
    PromoteGate& g = promote_[own.id];
    auto request = std::make_shared<PromoteRequest>();
    request->root = h;
    request->reserveRoot = reserveRoot;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.token != own || !g.accepting) return PromoteOutcome::Failed;
        g.requests.push_back(request);
        g.requested = true;
    }
    promoteFlag_[own.id].store(1, std::memory_order_release);
    std::unique_lock<std::mutex> lk(g.mu);
    g.cv.wait(lk, [&] {
        return request->outcome != PromoteOutcome::Pending ||
               g.token != own || !g.accepting;
    });
    return request->outcome;
}

inline void Runtime::waitUntilNotPromoting(ObjHandle h) {
    const OwnerToken own = pool_.ownerTokenAt(h.index);
    if (own.id >= CellPool::kMaxOwners) return;
    PromoteGate& g = promote_[own.id];
    std::unique_lock<std::mutex> lk(g.mu);
    g.cv.wait(lk, [&] {
        return !pool_.valid(h) || pool_.regimeOf(h) != Regime::Promoting;
    });
}

// ---- Mutator method definitions (need a complete Runtime) ----------------

inline ObjHandle Mutator::alloc() {
    if (adaptive()) return allocShared();   // generic alloc is SHARED in adaptive
#if AARC_PROFILE
    ++prof_.ops;
    const bool samp = profSample(prof_.poolOps);
    ProfTsc tPool(samp);
#endif
    // Sync: plain global allocation; Async: reuse our own recycle pile first
    // (birth == death locality).
    ObjHandle h = (owner_ != kNoOwnerToken)
                      ? rt_.pool_.allocOwned(owner_)
                      : rt_.pool_.alloc();
#if AARC_PROFILE
    prof_.poolAlloc += tPool.stop();
#endif
    if (h == kNullHandle) return h;
    submit({GCOp::RootAdd, kNullHandle, h});
#if AARC_PROFILE
    ProfTsc tBag(profSample(prof_.bagOps));
#endif
    roots_.add(packHandle(h));
#if AARC_PROFILE
    prof_.rootBag += tBag.stop();
#endif
    return h;
}

inline ObjHandle Mutator::allocLocal() {
    // closed_ is owner-thread-local and close() is idempotent, so it fully
    // answers "may I still allocate LOCAL?" without a registry lookup.
    if (!adaptive() || closed_) return kNullHandle;
    maybeServe();                           // owner safe point: drain requests first
    ObjHandle h = rt_.pool_.allocLocalRegime(owner_);
    if (h == kNullHandle) return h;
    local_->process({GCOp::RootAdd, kNullHandle, h});
    roots_.add(packHandle(h));
#ifdef AARC_CONSERVATIVE
    rt_.comps_.addLocal(owner_, h.index);
#endif
    return h;
}

inline ObjHandle Mutator::allocShared() {
    if (!adaptive()) return alloc();        // legacy path (Sync/Async)
    maybeServe();
    ObjHandle h = rt_.pool_.allocSharedRegime(owner_);
    if (h == kNullHandle) return h;
    submit({GCOp::RootAdd, kNullHandle, h});
    roots_.add(packHandle(h));
    return h;
}

inline void Mutator::flush() {
    if (slot_ && slot_->batchN) rt_.flushSlot(slot_);
}

inline void Mutator::willPark() { flush(); }

inline void Mutator::maybeServe() {
    if (adaptive() && rt_.promotePending(owner_)) {
        flush();                 // the worker must see our shared-side ops first
        rt_.servePromotions(owner_, *local_);
    }
}

inline void Mutator::close() {
    if (closed_) return;
    closed_ = true;
    if (sync_ || owner_ == kNoOwnerToken) return;
    flush();                     // never leave staged messages behind
    if (adaptive() && local_) {
        rt_.closeOwner(owner_, local_.get(), slot_, roots_);
        roots_.reset();
        return;
    }
    if (rt_.ownerRegistry_.beginClose(owner_)) {
        roots_.each([&](uint64_t key, uint32_t count) {
            for (uint32_t c = 0; c < count; ++c)
                rt_.publish(slot_, {GCOp::RootRemove, kNullHandle,
                                     ObjHandle{uint32_t(key >> 32), uint32_t(key)}});
        });
        roots_.reset();
        rt_.awaitQuiesce();
        rt_.ownerRegistry_.release(owner_);
    }
}

inline uint64_t Mutator::pollPromotions() {
    if (!adaptive()) return 0;
    flush();                     // a parked owner must not hold the cut open
    return rt_.servePromotions(owner_, *local_);
}

inline bool Mutator::rootAdd(ObjHandle h) {
    if (closed_ || !rt_.pool_.valid(h)) return false;
    const bool ok = adaptive()
                        ? adaptiveRootOp(GCOp::RootAdd, h)
                        : (submit({GCOp::RootAdd, kNullHandle, h}), true);
    if (ok) {
#if AARC_PROFILE
        ++prof_.ops;
        ProfTsc tBag(profSample(prof_.bagOps));
#endif
        roots_.add(packHandle(h));
#if AARC_PROFILE
        prof_.rootBag += tBag.stop();
#endif
    }
    return ok;
}

inline bool Mutator::rootRemove(ObjHandle h) {
#if AARC_PROFILE
    ++prof_.ops;
    {
        ProfTsc tPre(profSample(prof_.preOps));
        const bool ok = !closed_ && rt_.pool_.valid(h);
        prof_.prologue += tPre.stop();
        if (!ok) return false;
    }
#else
    if (closed_) return false;
    // M12b fast path: a key in this mutator's RootBag proves it added a root for
    // exactly this (index, generation), and a held root keeps the cell alive, so
    // the pool validity probe is redundant.  Keyed by generation, so a recycled
    // slot can never match.  Behind a switch because it trades a redundant
    // safety check for ~4ns/pair.
    if (rt_.fastRootValid()) {
        if (!roots_.removeOne(packHandle(h))) return false;
        submit({GCOp::RootRemove, kNullHandle, h});
        return true;
    }
    if (!rt_.pool_.valid(h)) return false;
#endif
#if AARC_PROFILE
    ProfTsc tBag(profSample(prof_.bagOps));
#endif
    if (!roots_.removeOne(packHandle(h))) return false;
#if AARC_PROFILE
    prof_.rootBag += tBag.stop();
#endif
    const bool ok = adaptive()
                        ? adaptiveRootOp(GCOp::RootRemove, h)
                        : (submit({GCOp::RootRemove, kNullHandle, h}), true);
    if (!ok) roots_.add(packHandle(h));   // roll back on a failed op
    return ok;
}

// Root op on a handle we do not own: promote it to SHARED first (M3).  A
// foreign-owner LOCAL cell is handed to its owner for promotion and we wait;
// a PROMOTING cell is waited on; once SHARED the op goes to the worker.
inline bool Mutator::adaptiveRootOp(GCOp op, ObjHandle h) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        maybeServe();                        // owner safe point first
        if (!rt_.pool_.valid(h)) return false;
        switch (classify(h)) {
            case Authority::MyLocal:         // regime LOCAL, owner == this mutator
                local_->process({op, kNullHandle, h});
                return true;
            case Authority::SharedWorker:
                submit({op, kNullHandle, h});
                return true;
            default: {                       // foreign LOCAL owner / PROMOTING
                const PromoteOutcome outcome =
                    promoteHandle(h, op == GCOp::RootAdd);
                if (outcome == PromoteOutcome::RootReserved) return true;
                if (outcome == PromoteOutcome::Failed) {
                    ++promotionRequired_;
                    return false;
                }
                break;
            }
        }
    }
    ++promotionRequired_;
    return false;
}

inline bool Mutator::edgeAdd(ObjHandle src, ObjHandle dst) {
#if AARC_PROFILE
    {
        ++prof_.ops;
        ProfTsc tPre(profSample(prof_.preOps));
        const bool ok = rt_.pool_.valid(src) && rt_.pool_.valid(dst);
        prof_.prologue += tPre.stop();
        if (!ok) return false;
    }
#else
    if (!rt_.pool_.valid(src) || !rt_.pool_.valid(dst)) return false;
#endif
    if (adaptive()) return adaptiveEdgeOp(GCOp::EdgeAdd, src, dst);
    submit({GCOp::EdgeAdd, src, dst});
    return true;
}

inline bool Mutator::edgeRemove(ObjHandle src, ObjHandle dst) {
    if (!rt_.pool_.valid(src) || !rt_.pool_.valid(dst)) return false;
    if (adaptive()) return adaptiveEdgeOp(GCOp::EdgeRemove, src, dst);
    submit({GCOp::EdgeRemove, src, dst});
    return true;
}

// Edge op across an authority boundary: promote the LOCAL side's component so
// no cross-regime edge survives (rule 6).  Order is only a throughput concern —
// every promotion moves exactly one owner's LOCAL component to the worker, and
// the loop re-checks until both endpoints are SHARED (submit) or both are this
// owner's LOCAL (inline).
inline bool Mutator::adaptiveEdgeOp(GCOp op, ObjHandle src, ObjHandle dst) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        maybeServe();
        if (!rt_.pool_.valid(src) || !rt_.pool_.valid(dst)) return false;
        const Authority sa = classify(src);
        const Authority da = classify(dst);
        if (sa == Authority::MyLocal && da == Authority::MyLocal) {
            local_->process({op, src, dst});
#ifdef AARC_CONSERVATIVE
            if (op == GCOp::EdgeAdd)
                rt_.comps_.merge(owner_, src.index, dst.index);
#endif
            return true;
        }
        if (sa == Authority::SharedWorker && da == Authority::SharedWorker) {
            submit({op, src, dst});
            return true;
        }
        // Promote whichever LOCAL endpoint blocks the edge, then retry.  A
        // LOCAL(src) -> SHARED(dst) edge means the local object reaches into the
        // shared world, so the local side is the one that must go SHARED.
        ObjHandle pick = kNullHandle;
        if (da == Authority::SharedWorker)      pick = src;   // my Local -> shared
        else if (sa == Authority::SharedWorker) pick = dst;   // shared -> Local dst
        else if (sa == Authority::MyLocal && da == Authority::Foreign)
            pick = dst;                                       // my Local -> foreign Local
        else if (da == Authority::MyLocal)      pick = dst;   // foreign/promoting src -> my Local
        else                                    pick = src;   // src is foreign/promoting
        if (promoteHandle(pick) == PromoteOutcome::Failed) {
            ++promotionRequired_;
            return false;
        }
    }
    ++promotionRequired_;
    return false;
}

// Make `h` SHARED before the caller operates on it (M3).  Three cases:
//   * already SHARED                     -> done
//   * PROMOTING (a hand-off is in flight)-> wait for its owner to commit
//   * LOCAL owned by THIS mutator        -> self-promotion: run the hand-off here
//   * LOCAL owned by another owner       -> register + block until they serve it
inline PromoteOutcome Mutator::promoteHandle(ObjHandle h, bool reserveRoot) {
    if (!adaptive()) return PromoteOutcome::Failed;
    for (int attempt = 0; attempt < 16; ++attempt) {
        if (!rt_.pool_.valid(h)) return PromoteOutcome::Failed;
        const OwnerToken own = rt_.pool_.ownerTokenAt(h.index);
        const Regime r = rt_.pool_.regimeOf(h);
        if (r == Regime::Shared) return PromoteOutcome::Shared;
        // Every branch below either blocks or hands authority to another
        // engine, so our staged shared-side ops must be published first.
        flush();
        if (r == Regime::Promoting) { rt_.waitUntilNotPromoting(h); continue; }
        if (own == owner_) {
            rt_.promoteLocalComponent(owner_, *local_, std::vector<ObjHandle>{h});
            continue;
        }
        return rt_.promoteForeign(h, owner_, reserveRoot);
    }
    return PromoteOutcome::Failed;
}

inline Mutator::Authority Mutator::classify(ObjHandle h) const {
    // Caller has already checked pool_.valid(h).
    const Regime r = rt_.pool_.regimeOf(h);
    if (r == Regime::Shared) return Authority::SharedWorker;
    if (r == Regime::Local && rt_.pool_.ownerTokenAt(h.index) == owner_)
        return Authority::MyLocal;
    return Authority::Foreign;   // another owner's LOCAL, or PROMOTING (frozen)
}

inline void Mutator::collectLocal() {
    if (!adaptive()) return;
    maybeServe();                 // don't sweep cells a requester is escaping
    flush();                      // publish shared-side ops before parking
    local_->sweep();
}

inline void Runtime::writeToSpan(ProducerSlot* slot, const GCMessage& m) {
    if (slot->spanLeft == 0) {
        // Publish the open run first, then reserve a new one.  The batch stays
        // open (pubSeq odd), so the worker may drain these but never sweep.
        commitStaged(slot);
#if AARC_PROFILE
        ProfTsc tSpin(true);
#endif
        for (;;) {
            const size_t n = slot->queue.beginSpan(kMaxSpan);
            if (n) {
                slot->spanPtr = slot->queue.spanPtr();
                slot->spanLeft = static_cast<uint32_t>(n);
                break;
            }
#if AARC_ENABLE_STATS
            spins_.fetch_add(1, std::memory_order_relaxed);
#endif
            std::this_thread::yield();
        }
#if AARC_PROFILE
        slot->profSpin += tSpin.stop();
#endif
    }
    *slot->spanPtr++ = m;      // direct: this IS the ring slot, no second copy
    --slot->spanLeft;
    ++slot->spanN;
}

inline void Runtime::commitStaged(ProducerSlot* slot) {
    if (slot->spanN) {
        slot->queue.commitSpan(slot->spanN);
        slot->spanN = 0;
    }
}

inline void Runtime::publish(ProducerSlot* slot, const GCMessage& message) {
#if AARC_PROFILE
    ProfTsc tStage(profSample(slot->profOps));
#endif
    // Open the publication window on the first staged message: while the batch
    // is non-empty the worker must treat this producer's cut as unstable, since
    // a staged message is a reference it cannot see yet.
    if (slot->batchN == 0)
        slot->pubSeq.fetch_add(1, std::memory_order_seq_cst);
    if (fold_ || !spanPath_) {
        slot->batch[slot->batchN++] = message;   // staged (folded/pushed at flush)
    } else {
        writeToSpan(slot, message);              // zero-copy direct into the ring
        ++slot->batchN;
    }
    const bool full = slot->batchN >= slot->batchLimit;
#if AARC_PROFILE
    slot->profStage += tStage.stop();
#endif
    if (full) flushSlot(slot);
}

inline uint32_t Runtime::foldBatch(ProducerSlot* slot) {
    const uint32_t n = slot->batchN;
    ProducerSlot::FoldEntry* tab = slot->foldTab;
    constexpr uint32_t kMask = ProducerSlot::kFoldSlots - 1;
    uint32_t nUsed = 0;
    uint32_t out = 0;

    // Single pass: a root op is emitted at its FIRST occurrence and the slot is
    // patched in place as later ops on the same handle change the net delta.
    // The place in the order is irrelevant for correctness -- all of a handle's
    // root ops in this batch collapse into that one message, so its refcount is
    // the same whenever the message is applied, and the other handle's rows are
    // independent.
    for (uint32_t i = 0; i < n; ++i) {
        GCMessage m = slot->batch[i];
        ++slot->foldIn[static_cast<int>(m.op)];
        if (m.op != GCOp::RootAdd && m.op != GCOp::RootRemove) {
            slot->folded[out++] = m;          // edges are never folded (v1)
            continue;
        }
        const uint64_t key = packHandle(m.b);
        uint32_t h = static_cast<uint32_t>(ObjHandleHash{}(m.b)) & kMask;
        for (;;) {
            ProducerSlot::FoldEntry& e = tab[h];
            if (e.key == key) {
                e.delta += (m.op == GCOp::RootAdd) ? 1 : -1;
                GCMessage& em = slot->folded[e.outPos];
                if (e.delta > 0) {
                    em.op = GCOp::RootAdd;
                    em.count = static_cast<uint32_t>(e.delta);
                } else if (e.delta < 0) {
                    em.op = GCOp::RootRemove;
                    em.count = static_cast<uint32_t>(-e.delta);
                } else {
                    em.op = GCOp::RootCheck;  // balanced pair
                    em.count = 0;
                }
                break;
            }
            if (e.key == ProducerSlot::kNoFoldKey) {
                e.key = key;
                e.delta = (m.op == GCOp::RootAdd) ? 1 : -1;
                e.outPos = out;
                slot->foldUsed[nUsed++] = h;
                slot->folded[out++] = m;      // count == 1 as staged
                break;
            }
            h = (h + 1) & kMask;
        }
    }
    for (uint32_t i = 0; i < out; ++i)
        ++slot->foldOut[static_cast<int>(slot->folded[i].op)];
    for (uint32_t k = 0; k < nUsed; ++k)
        tab[slot->foldUsed[k]].key = ProducerSlot::kNoFoldKey;
    return out;
}

inline void Runtime::flushSlot(ProducerSlot* slot) {
    uint32_t n = slot->batchN;
    if (n == 0) return;
#if AARC_PROFILE
    ProfTsc tFlush(true);
    ++slot->profFlushes;
#endif
    if (fold_) {
        // Folding needs a staging buffer: fold the batch, then append the folded
        // messages through the same zero-copy span path.
        n = foldBatch(slot);
        for (uint32_t i = 0; i < n; ++i) writeToSpan(slot, slot->folded[i]);
        commitStaged(slot);
    } else if (!spanPath_) {
        for (uint32_t i = 0; i < n; ++i) {
            while (!slot->queue.tryPush(slot->batch[i])) {
#if AARC_ENABLE_STATS
                spins_.fetch_add(1, std::memory_order_relaxed);
#endif
                std::this_thread::yield();
            }
        }
    } else {
        // Messages are already in the ring; publishing the run makes them
        // visible.  A message may be visible to the worker before the window
        // closes; that is the existing "drain without reclaim" behaviour.
        commitStaged(slot);
    }
#ifdef AARC_TEST_HOOKS
    if (testHooks_.afterQueuePush) testHooks_.afterQueuePush();
#endif
    slot->batchN = 0;
    slot->pubSeq.fetch_add(1, std::memory_order_seq_cst);   // close (even)
    slot->batchesPublished++;
    slot->msgsPublished += n;
    batches_.fetch_add(1, std::memory_order_relaxed);
    msgsPublished_.fetch_add(n, std::memory_order_relaxed);
    workEpoch_.fetch_add(1, std::memory_order_release);
    wakeWorker();
#if AARC_PROFILE
    slot->profFlush += tFlush.stop();
#endif
}

inline void Runtime::flushOwnBatches() {
    const std::thread::id me = std::this_thread::get_id();
    ProducerSlot* mine[CellPool::kMaxOwners];
    size_t k = 0;
    {
        std::lock_guard<std::mutex> lk(slotsMu_);
        for (auto& slot : slots_) {
            if (slot->producerThread == me && slot->batchN) {
                if (k < CellPool::kMaxOwners) mine[k++] = slot.get();
                else flushSlot(slot.get());     // overflow: flush under the lock
            }
        }
    }
    for (size_t i = 0; i < k; ++i) flushSlot(mine[i]);
}

inline void Mutator::submit(const GCMessage& m) {
    if (sync_) {
        rt_.engine_.process(m);
        return;
    }
    rt_.publish(slot_, m);
}

}} // namespace loxis::aarc
