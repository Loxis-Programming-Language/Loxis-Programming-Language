// AARC cycle layer: Tarjan SCC component finder.
//
// Given a set of *suspect* cell indexes, find the strongly connected
// components (SCCs) of the subgraph reachable from them by following live
// outgoing edges.  The engine then uses these components to compute the
// external-incoming-reference count (ECR) of each component and reclaim the
// ones with ECR == 0 (Todo_AddGC.md 2.2).
//
// This is deliberately a proper SCC algorithm (Tarjan), not a "returned to the
// start of the DFS" heuristic — a back edge alone does not tell you the full
// garbage cycle membership (Todo_AddGC.md 2.2).
//
// Iterative (explicit frame stack) so deep/large reachable graphs do not blow
// the native call stack.  Scratch arrays are reused across sweeps.

#pragma once

#include "../core/cell.hpp"
#include "../arc/ref_table.hpp"
#include <cstdint>
#include <vector>

namespace loxis { namespace aarc {

// `Cells` is the object source the detector queries for liveness.  CellPool is
// the in-tree storage; a host VM can substitute its own (see AarcCellSource in
// the Loxis VM) without the algorithm knowing anything about it.
template <class Cells>
class CycleDetectorT {
public:
    explicit CycleDetectorT(uint32_t capacity)
        : disc_(capacity, 0),
          low_(capacity, 0),
          on_(capacity, 0),
          scc_(capacity, kUnvisited) {
        disc_.shrink_to_fit();
        low_.shrink_to_fit();
        on_.shrink_to_fit();
        scc_.shrink_to_fit();
    }

    CycleDetectorT(const CycleDetectorT&) = delete;
    CycleDetectorT& operator=(const CycleDetectorT&) = delete;

    // Returns the number of SCCs found.  Every node reachable from `roots`
    // (via live outgoing edges) is assigned an scc id in [0, count) and listed
    // exactly once in `visitedOut`.  `roots` must not contain duplicates.
    //
    // Contract: call release(visitedOut) once you are done reading sccOf(), so
    // the scratch state is clean for the next findComponents() call.
    uint32_t findComponents(const Cells& pool, const RefTable& tab,
                            const std::vector<uint32_t>& roots,
                            std::vector<uint32_t>& visitedOut) {
        visitedOut.clear();
        time_ = 0;
        comps_ = 0;
        tarjan_.clear();
        frames_.clear();
        frameNext_.clear();

        skippedDisc_ = 0;
        skippedDead_ = 0;
        for (uint32_t root : roots) {
            if (disc_[root] != 0) { ++skippedDisc_; continue; }
            if (!pool.isLiveAt(root)) { ++skippedDead_; continue; }
            visitFrom(root, pool, tab, visitedOut);
        }
        return comps_;
    }

    // Clear per-node scratch for the given visited nodes (engine calls this
    // once it no longer needs sccOf()).  Cheap: proportional to the scanned
    // region, not the pool capacity.
    void release(const std::vector<uint32_t>& visited) {
        for (uint32_t i : visited) {
            disc_[i] = 0;
            low_[i] = 0;
            on_[i] = 0;
            scc_[i] = kUnvisited;
        }
    }

    // Why roots were dropped by the last findComponents() call (diagnostics).
    uint64_t skippedAlreadyDiscovered() const { return skippedDisc_; }
    uint64_t skippedDead() const { return skippedDead_; }

    // scc id of a node, or kUnvisited if it was not reached.
    uint32_t sccOf(uint32_t i) const { return scc_[i]; }
    bool isVisited(uint32_t i) const { return scc_[i] != kUnvisited; }

private:
    static constexpr uint32_t kUnvisited = 0xFFFFFFFFu;

    // Iterative Tarjan from one root.
    void visitFrom(uint32_t root, const Cells& pool, const RefTable& tab,
                   std::vector<uint32_t>& visitedOut) {
#ifdef AARC_CONSERVATIVE
        // Forward-only table: outgoing edges are a per-slot vector.  The frame
        // stores the next vector index (valid because no edge mutation happens
        // during a sweep).
        frames_.push_back(root);
        frameNext_.push_back(0);
        while (!frames_.empty()) {
            const uint32_t u = frames_.back();
            if (disc_[u] == 0) {
                disc_[u] = low_[u] = ++time_;
                on_[u] = 1;
                tarjan_.push_back(u);
                visitedOut.push_back(u);
            }
            const auto& outs = tab.outs(u);
            if (frameNext_.back() < outs.size()) {
                const ObjHandle vh = outs[frameNext_.back()];
                ++frameNext_.back();
                if (!pool.valid(vh)) continue;   // stale / recycled target
                const uint32_t v = vh.index;
                if (disc_[v] == 0) {
                    frames_.push_back(v);
                    frameNext_.push_back(0);
                } else if (on_[v] && disc_[v] < low_[u]) {
                    low_[u] = disc_[v];
                }
            } else {
                finishNode(u);
            }
        }
#else
        // Reverse table: outgoing edges are an EdgeId-linked list.
        frames_.push_back(root);
        frameNext_.push_back(RefTable::kNoEdge);
        while (!frames_.empty()) {
            const uint32_t u = frames_.back();
            if (disc_[u] == 0) {
                disc_[u] = low_[u] = ++time_;
                on_[u] = 1;
                tarjan_.push_back(u);
                visitedOut.push_back(u);
                frameNext_.back() = tab.firstOut(u);
            }
            if (frameNext_.back() != RefTable::kNoEdge) {
                const RefTable::EdgeId edge = frameNext_.back();
                frameNext_.back() = tab.nextOut(edge);
                const ObjHandle vh = tab.edgeTarget(edge);
                if (!pool.valid(vh)) continue;   // stale / recycled target
                const uint32_t v = vh.index;
                if (disc_[v] == 0) {
                    frames_.push_back(v);
                    frameNext_.push_back(RefTable::kNoEdge);
                } else if (on_[v] && disc_[v] < low_[u]) {
                    low_[u] = disc_[v];
                }
            } else {
                finishNode(u);
            }
        }
#endif
    }

private:
    // Pop the Tarjan stack up to `u` when u closes an SCC, then fold low-link up
    // to its DFS parent.
    void finishNode(uint32_t u) {
        frames_.pop_back();
        frameNext_.pop_back();
        if (low_[u] == disc_[u]) {
            for (;;) {
                const uint32_t w = tarjan_.back();
                tarjan_.pop_back();
                on_[w] = 0;
                scc_[w] = comps_;
                if (w == u) break;
            }
            ++comps_;
        }
        if (!frames_.empty()) {
            const uint32_t p = frames_.back();
            if (low_[u] < low_[p]) low_[p] = low_[u];
        }
    }

    std::vector<uint32_t> disc_;    // discovery time, 0 == unvisited
    std::vector<uint32_t> low_;     // low-link value
    std::vector<uint8_t> on_;       // on the Tarjan stack
    std::vector<uint32_t> scc_;     // component id (kUnvisited until found)

    std::vector<uint32_t> tarjan_;       // node stack
    std::vector<uint32_t> frames_;       // explicit DFS frame: node
    std::vector<uint32_t> frameNext_;    // ... and next outgoing index

    uint32_t time_ = 0;
    uint32_t comps_ = 0;
    uint64_t skippedDisc_ = 0;
    uint64_t skippedDead_ = 0;
};

using CycleDetector = CycleDetectorT<CellPool>;

}} // namespace loxis::aarc
