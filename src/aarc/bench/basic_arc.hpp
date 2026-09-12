// Minimal synchronous, atomic reference-counting baseline, used only as a
// comparison harness for the AARC benchmarks (Todo_AddGC.md 10).
//
// Semantics (the "ordinary ARC" everyone compares against):
//   * alloc            -> refcount 1
//   * retain / addEdge -> atomic fetch_add
//   * release          -> atomic fetch_sub; on reaching 0 the releasing thread
//                         immediately frees the cell and recursively releases
//                         its children (the free list is thread-safe).
//   * cycles are NEVER reclaimed (this is exactly the classic ARC limitation).
//
// Precondition for safety: an object's children list is written only while the
// builder exclusively owns the object (before it is shared), and read only by
// the single thread whose release takes the count to zero — the standard ARC
// construction-before-publish rule.

#pragma once

#include "../core/cell.hpp"
#include <atomic>
#include <vector>

namespace loxis { namespace aarc {

class BasicArc {
public:
    explicit BasicArc(uint32_t capacity)
        : pool_(capacity), count_(capacity), kids_(capacity) {}

    uint32_t liveCells() const { return pool_.used(); }
    uint64_t errorCount() const { return pool_.errorCount(); }

    ObjHandle alloc() {
        ObjHandle h = pool_.alloc();
        if (h == kNullHandle) return h;
        count_[h.index].store(1, std::memory_order_relaxed);
        kids_[h.index].clear();
        return h;
    }

    void retain(ObjHandle h) {
        count_[h.index].fetch_add(1, std::memory_order_relaxed);
    }

    // parent is exclusively owned by the caller; child may already be shared.
    void addEdge(ObjHandle parent, ObjHandle child) {
        kids_[parent.index].push_back(child);
        retain(child);
    }

    void release(ObjHandle h) {
        const int64_t prev = count_[h.index].fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) reclaim(h);
    }

private:
    void reclaim(ObjHandle h) {
        // read the child list, then free.  Recursion depth == graph depth.
        const auto children = kids_[h.index];
        for (const ObjHandle& c : children) {
            if (c.index != h.index) release(c);   // (self-edges are never freed)
        }
        kids_[h.index].clear();
        pool_.free(h);
    }

    CellPool pool_;
    std::vector<std::atomic<int64_t>> count_;
    std::vector<std::vector<ObjHandle>> kids_;
};

}} // namespace loxis::aarc
