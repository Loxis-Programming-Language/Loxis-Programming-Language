// AARC arc layer: reference counts plus persistent bidirectional adjacency.
//
// Every logical object edge is one stable EdgeRec in a chunked arena.  The
// record is linked into both the source's outgoing list and the target's
// incoming list.  EdgeId values stay stable when the arena grows; freed records
// are recycled through a tagged lock-free free list.  Duplicate (src,dst) edges
// are distinct records (multiset semantics).
//
// Adaptive mode has multiple single-writer graph authorities.  Their RefTable
// rows are disjoint, but they allocate edge records concurrently.  The arena
// therefore uses an atomic two-level block directory and never resizes a shared
// vector.  Installing a new segment/block is a one-time CAS; normal allocation
// is an atomic id/free-list operation.

#pragma once

#include "../core/cell.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace loxis { namespace aarc {

class RefTable {
public:
    using EdgeId = uint32_t;
    static constexpr EdgeId kNoEdge = 0xFFFFFFFFu;

private:
    struct Rec {
        int64_t root = 0;
        int64_t edge = 0;
        EdgeId outHead = kNoEdge;
        EdgeId inHead = kNoEdge;
        uint32_t outCount = 0;
        uint32_t inCount = 0;
    };

    struct EdgeRec {
        ObjHandle src;
        ObjHandle dst;
        EdgeId prevOut = kNoEdge;
        EdgeId nextOut = kNoEdge;
        EdgeId prevIn = kNoEdge;
        EdgeId nextIn = kNoEdge;
        EdgeId nextFree = kNoEdge;
        bool live = false;
    };

    static constexpr uint32_t kBlockSize = 1024;
    static constexpr uint32_t kBlocksPerSegment = 1024;
    static constexpr uint32_t kMaxSegments = 4096;
    using Block = std::array<EdgeRec, kBlockSize>;

    struct Segment {
        Segment() {
            for (auto& block : blocks) block.store(nullptr, std::memory_order_relaxed);
        }
        std::array<std::atomic<Block*>, kBlocksPerSegment> blocks;
    };

public:
    class AdjIterator {
    public:
        ObjHandle operator*() const {
            const EdgeRec& edge = table_->at(id_);
            return incoming_ ? edge.src : edge.dst;
        }
        AdjIterator& operator++() {
            const EdgeRec& edge = table_->at(id_);
            id_ = incoming_ ? edge.nextIn : edge.nextOut;
            return *this;
        }
        bool operator!=(const AdjIterator& other) const { return id_ != other.id_; }

    private:
        friend class RefTable;
        AdjIterator(const RefTable* table, EdgeId id, bool incoming)
            : table_(table), id_(id), incoming_(incoming) {}
        const RefTable* table_;
        EdgeId id_;
        bool incoming_;
    };

    class AdjRange {
    public:
        AdjIterator begin() const { return AdjIterator(table_, head_, incoming_); }
        AdjIterator end() const { return AdjIterator(table_, kNoEdge, incoming_); }
        size_t size() const { return count_; }
        bool empty() const { return count_ == 0; }

    private:
        friend class RefTable;
        AdjRange(const RefTable* table, EdgeId head, uint32_t count, bool incoming)
            : table_(table), head_(head), count_(count), incoming_(incoming) {}
        const RefTable* table_;
        EdgeId head_;
        uint32_t count_;
        bool incoming_;
    };

    explicit RefTable(uint32_t capacity) : rec_(capacity) {
        for (auto& segment : segments_)
            segment.store(nullptr, std::memory_order_relaxed);
    }

    ~RefTable() {
        for (auto& segmentSlot : segments_) {
            Segment* segment = segmentSlot.load(std::memory_order_relaxed);
            if (!segment) continue;
            for (auto& blockSlot : segment->blocks)
                delete blockSlot.load(std::memory_order_relaxed);
            delete segment;
        }
    }

    RefTable(const RefTable&) = delete;
    RefTable& operator=(const RefTable&) = delete;

    int64_t root(uint32_t i) const { return rec_[i].root; }
    int64_t edge(uint32_t i) const { return rec_[i].edge; }
    int64_t total(uint32_t i) const { return rec_[i].root + rec_[i].edge; }

    bool hasOutgoing(uint32_t i) const { return rec_[i].outCount != 0; }
    AdjRange outs(uint32_t i) const {
        return AdjRange(this, rec_[i].outHead, rec_[i].outCount, false);
    }
    AdjRange ins(uint32_t i) const {
        return AdjRange(this, rec_[i].inHead, rec_[i].inCount, true);
    }
    EdgeId firstOut(uint32_t i) const { return rec_[i].outHead; }
    EdgeId nextOut(EdgeId id) const { return at(id).nextOut; }
    ObjHandle edgeTarget(EdgeId id) const { return at(id).dst; }

    // `n` is the folded multiplicity (1 = one op).  subRoot never drives the
    // count negative and reports failure when the row held fewer roots.
    void addRoot(uint32_t i, uint32_t n = 1) { rec_[i].root += int64_t(n); }
    bool subRoot(uint32_t i, uint32_t n = 1) {
        if (rec_[i].root < int64_t(n)) { rec_[i].root = 0; return false; }
        rec_[i].root -= int64_t(n);
        return true;
    }

    void addEdgeTo(uint32_t i) { ++rec_[i].edge; }
    bool subEdgeTo(uint32_t i) {
        if (rec_[i].edge <= 0) return false;
        --rec_[i].edge;
        return true;
    }

    void addOut(ObjHandle src, ObjHandle dst) {
        const EdgeId id = allocateEdge();
        EdgeRec& e = at(id);
        e.src = src;
        e.dst = dst;
        e.live = true;

        Rec& source = rec_[src.index];
        e.nextOut = source.outHead;
        if (e.nextOut != kNoEdge) at(e.nextOut).prevOut = id;
        source.outHead = id;
        ++source.outCount;

        Rec& target = rec_[dst.index];
        e.nextIn = target.inHead;
        if (e.nextIn != kNoEdge) at(e.nextIn).prevIn = id;
        target.inHead = id;
        ++target.inCount;
    }

    // Remove one matching occurrence from the source multiset.  Finding it is
    // O(out-degree); once found, unlinking from both lists is O(1).
    bool removeOut(ObjHandle src, ObjHandle dst) {
        for (EdgeId id = rec_[src.index].outHead; id != kNoEdge;
             id = at(id).nextOut) {
            const EdgeRec& e = at(id);
            if (e.src == src && e.dst == dst) {
                unlink(id);
                return true;
            }
        }
        return false;
    }

    // Remove every outgoing edge of a dying source.  The caller has already
    // adjusted target refcounts; this operation only removes graph metadata.
    void clearOuts(uint32_t i) {
        EdgeId id = rec_[i].outHead;
        while (id != kNoEdge) {
            const EdgeId next = at(id).nextOut;
            unlink(id);
            id = next;
        }
    }

    // A slot can only be reset once no live incoming edges target it.  Cycle
    // reclamation removes all member outgoing records before resetting members.
    void resetSlot(uint32_t i) {
        clearOuts(i);
        rec_[i] = Rec{};
    }

    uint64_t liveEdges() const { return liveEdges_.load(std::memory_order_relaxed); }

private:
    static uint64_t packFree(EdgeId id, uint32_t tag) {
        return (uint64_t(tag) << 32) | id;
    }
    static EdgeId freeId(uint64_t value) { return static_cast<EdgeId>(value); }
    static uint32_t freeTag(uint64_t value) { return static_cast<uint32_t>(value >> 32); }

    Block* ensureBlock(EdgeId id) {
        const uint32_t blockIndex = id / kBlockSize;
        const uint32_t segmentIndex = blockIndex / kBlocksPerSegment;
        const uint32_t blockOffset = blockIndex % kBlocksPerSegment;
        if (segmentIndex >= kMaxSegments) throw std::bad_alloc();

        Segment* segment = segments_[segmentIndex].load(std::memory_order_acquire);
        if (!segment) {
            std::unique_ptr<Segment> fresh(new Segment());
            Segment* expected = nullptr;
            if (segments_[segmentIndex].compare_exchange_strong(
                    expected, fresh.get(), std::memory_order_release,
                    std::memory_order_acquire))
                segment = fresh.release();
            else
                segment = expected;
        }

        Block* block = segment->blocks[blockOffset].load(std::memory_order_acquire);
        if (!block) {
            std::unique_ptr<Block> fresh(new Block());
            Block* expected = nullptr;
            if (segment->blocks[blockOffset].compare_exchange_strong(
                    expected, fresh.get(), std::memory_order_release,
                    std::memory_order_acquire))
                block = fresh.release();
            else
                block = expected;
        }
        return block;
    }

    EdgeRec& at(EdgeId id) {
        const uint32_t blockIndex = id / kBlockSize;
        Segment* segment = segments_[blockIndex / kBlocksPerSegment].load(
            std::memory_order_acquire);
        Block* block = segment->blocks[blockIndex % kBlocksPerSegment].load(
            std::memory_order_acquire);
        return (*block)[id % kBlockSize];
    }
    const EdgeRec& at(EdgeId id) const {
        const uint32_t blockIndex = id / kBlockSize;
        const Segment* segment = segments_[blockIndex / kBlocksPerSegment].load(
            std::memory_order_acquire);
        const Block* block = segment->blocks[blockIndex % kBlocksPerSegment].load(
            std::memory_order_acquire);
        return (*block)[id % kBlockSize];
    }

    EdgeId popFree() {
        uint64_t head = freeHead_.load(std::memory_order_acquire);
        for (;;) {
            const EdgeId id = freeId(head);
            if (id == kNoEdge) return kNoEdge;
            const EdgeId next = at(id).nextFree;
            const uint64_t desired = packFree(next, freeTag(head) + 1);
            if (freeHead_.compare_exchange_weak(
                    head, desired, std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return id;
        }
    }

    void pushFree(EdgeId id) {
        EdgeRec& e = at(id);
        uint64_t head = freeHead_.load(std::memory_order_acquire);
        do {
            e.nextFree = freeId(head);
        } while (!freeHead_.compare_exchange_weak(
            head, packFree(id, freeTag(head) + 1),
            std::memory_order_release, std::memory_order_acquire));
    }

    EdgeId allocateEdge() {
        EdgeId id = popFree();
        if (id == kNoEdge) {
            id = nextId_.fetch_add(1, std::memory_order_relaxed);
            if (id == kNoEdge) throw std::bad_alloc();
            ensureBlock(id);
        }
        at(id) = EdgeRec{};
        liveEdges_.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    void unlink(EdgeId id) {
        EdgeRec& e = at(id);
        if (!e.live) return;

        Rec& source = rec_[e.src.index];
        if (e.prevOut != kNoEdge) at(e.prevOut).nextOut = e.nextOut;
        else source.outHead = e.nextOut;
        if (e.nextOut != kNoEdge) at(e.nextOut).prevOut = e.prevOut;
        --source.outCount;

        Rec& target = rec_[e.dst.index];
        if (e.prevIn != kNoEdge) at(e.prevIn).nextIn = e.nextIn;
        else target.inHead = e.nextIn;
        if (e.nextIn != kNoEdge) at(e.nextIn).prevIn = e.prevIn;
        --target.inCount;

        e.live = false;
        liveEdges_.fetch_sub(1, std::memory_order_relaxed);
        pushFree(id);
    }

    std::vector<Rec> rec_;
    std::array<std::atomic<Segment*>, kMaxSegments> segments_;
    std::atomic<uint64_t> freeHead_{packFree(kNoEdge, 0)};
    std::atomic<EdgeId> nextId_{0};
    std::atomic<uint64_t> liveEdges_{0};
};

}} // namespace loxis::aarc
