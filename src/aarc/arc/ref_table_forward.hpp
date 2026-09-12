// Forward-only reference table (the CONSERVATIVE scheme's edge store).
//
// Exact reverse adjacency costs ~40 B/edge plus an incoming-list unlink on every
// EdgeRemove.  Conservative components make promotion correct with only a
// forward graph, so this variant stores the outgoing multiset as a plain
// per-slot vector of handles (~8 B/edge, no incoming list).  EdgeAdd is a push;
// EdgeRemove is a linear find+erase (pre-M6 cost profile).

#pragma once

#include "../core/cell.hpp"
#include <cstdint>
#include <vector>

namespace loxis { namespace aarc {

class RefTable {
    struct Rec {
        int64_t root = 0;
        int64_t edge = 0;
    };

public:
    explicit RefTable(uint32_t capacity) : rec_(capacity), outs_(capacity) {}

    int64_t root(uint32_t i) const { return rec_[i].root; }
    int64_t edge(uint32_t i) const { return rec_[i].edge; }
    int64_t total(uint32_t i) const { return rec_[i].root + rec_[i].edge; }

    bool hasOutgoing(uint32_t i) const { return !outs_[i].empty(); }
    const std::vector<ObjHandle>& outs(uint32_t i) const { return outs_[i]; }

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
        outs_[src.index].push_back(dst);
        ++liveEdges_;
    }

    // Remove one occurrence of dst from src's outgoing multiset.  Generation is
    // part of ObjHandle equality, so a recycled slot never matches a stale edge.
    bool removeOut(ObjHandle src, ObjHandle dst) {
        auto& v = outs_[src.index];
        for (size_t k = 0; k < v.size(); ++k) {
            if (v[k] == dst) {
                v.erase(v.begin() + static_cast<ptrdiff_t>(k));
                --liveEdges_;
                return true;
            }
        }
        return false;
    }

    void clearOuts(uint32_t i) {
        liveEdges_ -= outs_[i].size();
        outs_[i].clear();
    }

    void resetSlot(uint32_t i) {
        liveEdges_ -= outs_[i].size();
        rec_[i] = Rec{};
        outs_[i].clear();
    }

    uint64_t liveEdges() const { return liveEdges_; }

private:
    std::vector<Rec> rec_;
    std::vector<std::vector<ObjHandle>> outs_;
    uint64_t liveEdges_ = 0;
};

}} // namespace loxis::aarc
