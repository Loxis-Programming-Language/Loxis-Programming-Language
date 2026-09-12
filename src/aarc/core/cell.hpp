// AARC core: cell handle + fixed-size cell pool.
//
// An AARC *cell* is the smallest GC-managed unit (conceptually 9 bytes,
// see Todo_AddGC.md 2).  The payload is stored as raw bytes and exposed with
// accessors so that a natural C++ struct (which alignment would inflate to
// 12/16 bytes) is never used to represent it.
//
// A cell is referenced by a versioned handle  ObjHandle{index, generation}.
// The generation is bumped every time a slot is recycled, which makes stale
// handles / stale queued GC messages harmless: they never match a live slot
// of a *different* object (ABA protection, Todo_AddGC.md 1.3).
//
// Fast-path liveness (the hot per-message check) is lock-free: each slot keeps
// a single atomic<uint64_t> = (generation << 1) | live_bit.  alloc()/free()
// are the only writers of a slot's meta and each transition is ONE atomic
// store, so a concurrent reader doing an acquire load of
// `live_bit && generation == handle.gen` never sees a torn state.
//
// Reclamation locality (the key AARC allocator problem):
//   * Every cell remembers the producer id (`owner_`) that allocated it
//     (its "birth" thread).
//   * The collector frees a cell by routing it back to THAT producer's
//     per-owner recycle queue (single worker -> single-producer, owner is the
//     single consumer: an SPSC each).  If the owner's queue is full the cell
//     spills to the shared global free list instead (nothing is ever lost).
//   * allocOwned() pops the caller's own recycle queue first and only falls
//     back to the global (sharded) free list when empty.  So in steady state a
//     producer reuses the very cells its own objects just died in — locality
//     survives the asynchronous free, because the free is routed back to the
//     thread that allocates.
//   * Plain alloc() (baseline / sync) marks the cell kNoOwner, so free() puts
//     it straight back on the global list.
//
// Global free list is sharded (16 ranges, thread-affine pop + cross-shard
// steal) so cold/steal allocations do not all fight one lock.
//
// used() is computed by scanning the meta words (callers do it rarely);
// allocation itself carries no global counter update.
//
// Payload layout (per cell):  tag:1 byte, id:4 bytes, data:4 bytes.

#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <mutex>
#include <limits>
#include <atomic>
#include <cassert>
#include <functional>
#include <thread>

#include "spsc_queue.hpp"
#include "owner.hpp"

namespace loxis { namespace aarc {

// ---- cell payload layout ------------------------------------------------
constexpr uint32_t kCellBytes   = 9;
constexpr uint32_t kTagOffset   = 0;
constexpr uint32_t kIdOffset    = 1;
constexpr uint32_t kDataOffset  = 5;

// ---- versioned handle ---------------------------------------------------
constexpr uint32_t kNullIndex = 0xFFFFFFFFu;

struct ObjHandle {
    uint32_t index = kNullIndex;   // slot index in the pool; kNullIndex == null
    uint32_t gen   = 0;            // generation of that slot at handle creation time
};

constexpr ObjHandle kNullHandle{kNullIndex, 0u};

inline bool operator==(ObjHandle a, ObjHandle b) {
    return a.index == b.index && a.gen == b.gen;
}
inline bool operator!=(ObjHandle a, ObjHandle b) { return !(a == b); }

struct ObjHandleHash {
    size_t operator()(ObjHandle h) const noexcept {
        uint64_t x = (uint64_t(h.index) << 32) | h.gen;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};

// ---- regime (adaptive rc: LOCAL inline / SHARED async) --------------------
// v1 adaptive design (see docs/adaptive-rc-design.md).  In M1 the regime
// exists per-slot but is not yet exercised: a pool is all-LOCAL (sync) or
// all-SHARED (async) until M2 lets both engines coexist over one pool.
enum class Regime : uint8_t {
    Local,      // single owner thread, inline engine
    Promoting,  // ownership hand-off in progress (frozen)
    Shared      // owned by the async worker
};

// ---- fixed-size cell pool ------------------------------------------------
class CellPool {
public:
    static constexpr uint32_t kMaxOwners = 64;
    static constexpr uint32_t kRecycleCap = 1024;   // per-owner recycle queue
    static constexpr uint32_t kRefill = 64;         // slots per bulk refill
    static constexpr uint16_t kNoOwner = 0xFFFFu;

    explicit CellPool(uint32_t capacity, Regime defaultRegime = Regime::Shared)
        : capacity_(capacity ? capacity : 1),
          payload_(size_t(capacity_) * kCellBytes, 0),
          meta_(capacity_),
          owner_(capacity_),
          ownerGen_(capacity_),
          regime_(capacity_),
          next_(capacity_),
          recycle_(kMaxOwners),
          local_(kMaxOwners),
          heads_(kShards, kFreeEnd),
          defaultRegime_(defaultRegime) {
        assert(capacity_ < kNullIndex);
        for (uint32_t i = 0; i < capacity_; ++i) {
            meta_[i].store(encode(1, false), std::memory_order_relaxed);
            owner_[i].store(kNoOwner, std::memory_order_relaxed);
            ownerGen_[i].store(0, std::memory_order_relaxed);
            regime_[i].store(static_cast<uint8_t>(defaultRegime), std::memory_order_relaxed);
        }
        for (uint32_t i = capacity_; i-- > 0;) {
            const uint32_t s = shardOf(i);
            next_[i] = heads_[s];
            heads_[s] = i;
        }
    }

    CellPool(const CellPool&) = delete;
    CellPool& operator=(const CellPool&) = delete;

    uint32_t capacity() const noexcept { return capacity_; }

    // Live-cell count: O(capacity) scan of the meta words.  Diagnostic only.
    uint32_t used() const {
        uint32_t u = 0;
        for (uint32_t i = 0; i < capacity_; ++i) {
            if (meta_[i].load(std::memory_order_acquire) & kLiveBit) ++u;
        }
        return u;
    }

    // A/B switch for the producer-local slot cache (M11b).
    void setLocalCache(bool v) { useLocalCache_ = v; }

    // Plain allocation (baseline / sync / no owner affinity).  Regime follows
    // the pool default (Local in sync runtimes, Shared elsewhere).
    ObjHandle alloc() { return allocInternal(kNoOwner); }

    // Allocate for producer `owner`: take from its recycle pile first (SPSC,
    // no cross-thread contention), then the shared free list.  Regime follows
    // the pool default.
    ObjHandle allocOwned(uint16_t owner) { return allocInto(owner, defaultRegime_, 0); }

    // Owner-aware allocation.  The generation is part of the authority identity
    // and is never inferred from a recycled numeric owner id.
    ObjHandle allocOwned(OwnerToken owner) {
        return allocInto(owner.id, defaultRegime_, owner.generation);
    }

    // Adaptive (M2b): pre-classified allocation.  allocLocalRegime hands the
    // cell to `owner`'s inline engine (regime LOCAL); allocSharedRegime hands
    // it to the async worker (regime SHARED).  Callers choose the authority up
    // front instead of promoting at runtime (design doc §7 M2).
    ObjHandle allocLocalRegime(uint16_t owner) { return allocInto(owner, Regime::Local, 0); }
    ObjHandle allocSharedRegime(uint16_t owner) { return allocInto(owner, Regime::Shared, 0); }
    ObjHandle allocLocalRegime(OwnerToken owner) {
        return allocInto(owner.id, Regime::Local, owner.generation);
    }
    ObjHandle allocSharedRegime(OwnerToken owner) {
        return allocInto(owner.id, Regime::Shared, owner.generation);
    }

    // Recycle a cell.  Guarded against double-free / stale-handle free (both
    // counted in errorCount()).  Routes to the owning producer's recycle queue
    // when it has room, else back to the shared free list.
    bool free(ObjHandle h) {
        if (h.index >= capacity_) { errs_.fetch_add(1, std::memory_order_relaxed); return false; }
        const uint32_t s = shardOf(h.index);
        std::lock_guard<std::mutex> lk(mu_[s]);
        const uint64_t m = meta_[h.index].load(std::memory_order_acquire);
        if ((m & kLiveBit) == 0) { errs_.fetch_add(1, std::memory_order_relaxed); return false; }
        if (static_cast<uint32_t>(m >> kGenShift) != h.gen) {
                                     errs_.fetch_add(1, std::memory_order_relaxed); return false; }
        const uint32_t owner = owner_[h.index].load(std::memory_order_relaxed);
        const Regime rg = static_cast<Regime>(
            regime_[h.index].load(std::memory_order_relaxed));
        const uint32_t gen = static_cast<uint32_t>(m >> kGenShift);
        if (gen == std::numeric_limits<uint32_t>::max()) {
            // retire the slot forever instead of wrapping generation
            meta_[h.index].store(encode(gen, false), std::memory_order_release);
            return true;
        }
        const uint32_t newGen = gen + 1;
        meta_[h.index].store(encode(newGen, false), std::memory_order_release);
        const ObjHandle freed{h.index, newGen};
        // Only the worker frees SHARED cells, so it is the *sole producer* of a
        // per-owner recycle pile.  LOCAL cells are freed inline by their own
        // owner thread; routing them into that pile would give it a second
        // producer (an SPSC violation), so they spill to the shared list.
        if (rg == Regime::Shared && owner < kMaxOwners &&
            recycle_[owner].tryPush(freed))
            return true;
        next_[h.index] = heads_[s];
        heads_[s] = h.index;                      // spill to shared free list
        return true;
    }

    // Move every leftover recycle-pile cell back onto the shared free list.
    // Called once at teardown (all producers finished, worker joined).
    void drainRecycleAll() {
        ObjHandle h;
        for (uint32_t o = 0; o < kMaxOwners; ++o) {
            while (recycle_[o].tryPop(h)) {
                const uint32_t s = shardOf(h.index);
                std::lock_guard<std::mutex> lk(mu_[s]);
                next_[h.index] = heads_[s];
                heads_[s] = h.index;
            }
        }
    }

    // ---- lock-free liveness checks (per-message hot path) ----------------
    bool valid(ObjHandle h) const {
        return h.index < capacity_ &&
               meta_[h.index].load(std::memory_order_acquire) == encode(h.gen, true);
    }
    bool isLiveAt(uint32_t idx) const {
        return idx < capacity_ &&
               (meta_[idx].load(std::memory_order_acquire) & kLiveBit) != 0;
    }
    uint32_t generationOf(uint32_t idx) const {
        return idx < capacity_
                   ? static_cast<uint32_t>(meta_[idx].load(std::memory_order_acquire) >> kGenShift)
                   : 0;
    }

    // ---- payload accessors (caller must hold a live handle) -------------
    uint8_t getTag(ObjHandle h) const { return raw(h.index)[kTagOffset]; }
    void setTag(ObjHandle h, uint8_t v) { raw(h.index)[kTagOffset] = v; }

    uint32_t getId(ObjHandle h) const {
        uint32_t v; std::memcpy(&v, raw(h.index) + kIdOffset, 4); return v;
    }
    void setId(ObjHandle h, uint32_t v) { std::memcpy(raw(h.index) + kIdOffset, &v, 4); }

    uint32_t getData(ObjHandle h) const {
        uint32_t v; std::memcpy(&v, raw(h.index) + kDataOffset, 4); return v;
    }
    void setData(ObjHandle h, uint32_t v) { std::memcpy(raw(h.index) + kDataOffset, &v, 4); }

    template <class F>
    void forEachLive(F&& cb) const {
        for (uint32_t i = 0; i < capacity_; ++i) {
            const uint64_t m = meta_[i].load(std::memory_order_acquire);
            if (m & kLiveBit)
                cb(ObjHandle{i, static_cast<uint32_t>(m >> kGenShift)});
        }
    }

    uint64_t errorCount() const { return errs_.load(std::memory_order_relaxed); }

    // ---- regime (adaptive-rc plumbing; see design doc) --------------------
    // Regime is stored atomically because promotion (M3) flips a LIVE slot
    // Local->Promoting->Shared on the owner thread while escaping threads read
    // it to dispatch — the bit must never tear and reads may legally lag.
    Regime regimeOf(ObjHandle h) const {
        return regimeAt(h.index);
    }
    void setRegime(ObjHandle h, Regime r) {
        if (h.index < capacity_)
            regime_[h.index].store(static_cast<uint8_t>(r), std::memory_order_relaxed);
    }
    Regime regimeAt(uint32_t idx) const {
        return idx < capacity_
                   ? static_cast<Regime>(regime_[idx].load(std::memory_order_relaxed))
                   : Regime::Shared;
    }

    // Owner id of the slot (kNoOwner for plain/global allocations).  Written
    // once at activate() before the live store and never rewritten while live,
    // but stored atomically because whole-pool scans (closure) may read the
    // owner_ of a slot another thread is concurrently activating.
    uint16_t ownerOf(ObjHandle h) const {
        return ownerAt(h.index);
    }
    uint16_t ownerAt(uint32_t idx) const {
        return idx < capacity_
                   ? owner_[idx].load(std::memory_order_relaxed)
                   : kNoOwner;
    }
    uint32_t ownerGenerationAt(uint32_t idx) const {
        return idx < capacity_ ? ownerGen_[idx].load(std::memory_order_relaxed) : 0;
    }
    OwnerToken ownerTokenAt(uint32_t idx) const {
        return OwnerToken{ownerAt(idx), ownerGenerationAt(idx)};
    }

private:
    // Producer-private free-slot cache, one per owner.  Written only by the
    // owner thread; refilled in bulk so the shared lists are touched once per
    // kRefill allocations instead of once per allocation.
    struct LocalCache {
        ObjHandle slots[kRefill];
        uint32_t n = 0;
    };

    static constexpr uint32_t kShards = 16;
    static constexpr uint64_t kLiveBit = 0x1ull;
    static constexpr uint32_t kGenShift = 1;
    static constexpr uint32_t kFreeEnd = 0xFFFFFFFFu;

    static uint64_t encode(uint32_t gen, bool live) {
        return (uint64_t(gen) << kGenShift) | (live ? kLiveBit : 0);
    }

    uint32_t shardOf(uint32_t idx) const {
        const uint32_t per = (capacity_ + kShards - 1) / kShards;
        const uint32_t s = idx / (per ? per : 1u);
        return s < kShards ? s : kShards - 1;
    }

    static uint32_t tlsShard() {
        thread_local int shard = -1;
        if (shard < 0) {
            const size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
            shard = static_cast<int>(h % kShards);
        }
        return static_cast<uint32_t>(shard);
    }

    ObjHandle popFrom(uint32_t s) {
        std::lock_guard<std::mutex> lk(mu_[s]);
        if (heads_[s] == kFreeEnd) return kNullHandle;
        const uint32_t idx = heads_[s];
        heads_[s] = next_[idx];
        return ObjHandle{idx, static_cast<uint32_t>(
            meta_[idx].load(std::memory_order_relaxed) >> kGenShift)};
    }

    // Pop a fresh slot from the shared free list and hand it to `owner`.
    ObjHandle allocInternal(uint16_t owner) { return allocInto(owner, defaultRegime_, 0); }

    // Shared allocation core.  Order: owner-local cache -> owner recycle pile
    // (SPSC, worker is the producer) -> sharded global list.  The local cache and
    // both refills are BULK (kRefill slots per trip), so the per-object cost is a
    // producer-private pop instead of an atomic/mutex per object (M11b).
    ObjHandle allocInto(uint16_t owner, Regime r, uint32_t ownerGeneration) {
        if (useLocalCache_ && owner < kMaxOwners) {
            LocalCache& lc = local_[owner];
            if (lc.n == 0) refillLocal(owner, lc);
            if (lc.n) {
                const ObjHandle h = lc.slots[--lc.n];
                activate(h, owner, ownerGeneration, r);
                return h;
            }
        }
        uint32_t s = tlsShard();
        ObjHandle h = popFrom(s);
        if (h == kNullHandle) {
            for (uint32_t k = 1; k < kShards; ++k) {
                h = popFrom((s + k) % kShards);
                if (h != kNullHandle) break;
            }
        }
        if (h == kNullHandle) return kNullHandle;
        activate(h, owner, ownerGeneration, r);
        return h;
    }

    // Owner thread only: top the local cache up from its recycle pile, then from
    // the sharded global list, one lock acquisition per kRefill slots.
    void refillLocal(uint16_t owner, LocalCache& lc) {
        ObjHandle h;
        while (lc.n < kRefill && recycle_[owner].tryPop(h))
            lc.slots[lc.n++] = h;
        if (lc.n == kRefill) return;
        for (uint32_t k = 0; k < kShards && lc.n < kRefill; ++k) {
            const uint32_t s = (tlsShard() + k) % kShards;
            std::lock_guard<std::mutex> lk(mu_[s]);
            while (lc.n < kRefill && heads_[s] != kFreeEnd) {
                const uint32_t idx = heads_[s];
                heads_[s] = next_[idx];
                lc.slots[lc.n++] = ObjHandle{
                    idx, static_cast<uint32_t>(
                             meta_[idx].load(std::memory_order_relaxed) >> kGenShift)};
            }
        }
    }

    // `h` is a slot currently free (live=0) with generation h.gen.  Claim it
    // for `owner`: zero payload, record owner + regime, then publish live
    // (release).  owner_/regime_ are written before the release store, so a
    // reader that acquire-loads meta_ sees them (free(), regimeOf() rely on it).
    void activate(ObjHandle h, uint16_t owner, uint32_t ownerGeneration,
                  Regime r) {
        const uint32_t idx = h.index;
        std::memset(&payload_[size_t(idx) * kCellBytes], 0, kCellBytes);
        owner_[idx].store(owner, std::memory_order_relaxed);   // before release
        ownerGen_[idx].store(ownerGeneration, std::memory_order_relaxed);
        regime_[idx].store(static_cast<uint8_t>(r), std::memory_order_relaxed);
        meta_[idx].store(encode(h.gen, true), std::memory_order_release);
    }

    uint8_t* raw(uint32_t idx) const {
        return const_cast<uint8_t*>(&payload_[size_t(idx) * kCellBytes]);
    }

    uint32_t capacity_;
    std::vector<uint8_t> payload_;
    std::vector<std::atomic<uint64_t>> meta_;
    std::vector<std::atomic<uint16_t>> owner_;
    std::vector<std::atomic<uint32_t>> ownerGen_;
    std::vector<std::atomic<uint8_t>> regime_;  // LOCAL / PROMOTING / SHARED (M1)
    std::vector<uint32_t> next_;          // LIFO links, per-slot
    std::vector<SpscQueue<ObjHandle, kRecycleCap>> recycle_;   // one per owner
    std::vector<LocalCache> local_;
    std::vector<uint32_t> heads_;         // one free-list head per shard
    mutable std::mutex mu_[kShards];
    std::atomic<uint64_t> errs_{0};
    bool useLocalCache_ = true;
    Regime defaultRegime_;                // M1: pool-wide regime
};

}} // namespace loxis::aarc
