// Conservative "may-connect" LOCAL components (AARC_CONSERVATIVE scheme).
//
// Promotion correctness only needs: when X escapes, every LOCAL cell that has,
// or ever had, a connection to X must be promoted together so no live edge
// crosses LOCAL<->SHARED.  We maintain an *over-approximation* of connectivity:
//   - every allocLocal starts a fresh singleton component;
//   - every LOCAL<->LOCAL EdgeAdd merges the two components;
//   - EdgeRemove does NOT split (components only ever grow by union);
//   - freeing a member unlinks it (lists hold only live members).
//
// Because a component never splits, it is closed under live edges in both
// directions, so promoting every live member of seed X's component can only
// over-promote, never leave a cross-regime live edge.
//
// Concurrency: per-slot membership is one GLOBAL copy (indexed by slot) because
// per-owner copies would be 64x the pool.  A slot's membership is written only
// by its owner thread (disjoint slots); component headers are per-owner vectors,
// so each owner touches only its own header list.  No locks.

#pragma once

#include "../core/cell.hpp"
#include "../core/owner.hpp"

#include <cstdint>
#include <vector>

namespace loxis { namespace aarc {

class ConservativeComponents {
public:
    static constexpr uint32_t kNoComp = 0xFFFFFFFFu;

    explicit ConservativeComponents(uint32_t capacity)
        : compOf_(capacity, kNoComp),
          prev_(capacity, kNoComp),
          next_(capacity, kNoComp),
          ownerStores_(CellPool::kMaxOwners) {}

    // Owner-thread only.

    // allocLocal(h): a fresh singleton component for slot h.
    void addLocal(OwnerToken owner, uint32_t slot) {
        OwnerStore& store = storeOf(owner);
        const uint32_t comp = newHeader(store);
        Header& hdr = store.headers[comp];
        hdr.owner = owner;
        hdr.head = hdr.tail = slot;
        hdr.live = 1;
        compOf_[slot] = comp;
        prev_[slot] = kNoComp;
        next_[slot] = kNoComp;
    }

    // LOCAL->LOCAL EdgeAdd: union a's and b's components.
    void merge(OwnerToken owner, uint32_t a, uint32_t b) {
        const uint32_t ca = compOf_[a];
        const uint32_t cb = compOf_[b];
        if (ca == kNoComp || cb == kNoComp || ca == cb) return;
        OwnerStore& store = storeOf(owner);
        // Absorb cb into ca (ca keeps its header id).  Re-label cb's members.
        Header& ha = store.headers[ca];
        Header& hb = store.headers[cb];
        // Splice b's list onto a's tail.
        const uint32_t aTail = ha.tail;
        const uint32_t bHead = hb.head;
        if (aTail != kNoComp) next_[aTail] = bHead;
        if (bHead != kNoComp) prev_[bHead] = aTail;
        ha.tail = hb.tail;
        ha.live += hb.live;
        // Re-label every member that now belongs to ca.
        for (uint32_t m = bHead; m != kNoComp; m = next_[m]) compOf_[m] = ca;
        freeHeader(store, cb);
    }

    // Called by the owner's engine when a LOCAL cell is freed (list is
    // live-only, so this is an O(1) splice-out).
    void unlink(OwnerToken owner, uint32_t slot) {
        const uint32_t comp = compOf_[slot];
        if (comp == kNoComp) return;
        const uint32_t p = prev_[slot];
        const uint32_t n = next_[slot];
        OwnerStore& store = storeOf(owner);
        if (comp >= store.headers.size() || store.headers[comp].owner != owner)
            return;   // stale after a retired/reused component; slot will be reset
        Header& hdr = store.headers[comp];
        if (p != kNoComp) next_[p] = n;
        else hdr.head = n;
        if (n != kNoComp) prev_[n] = p;
        else hdr.tail = p;
        --hdr.live;
        compOf_[slot] = kNoComp;
        prev_[slot] = kNoComp;
        next_[slot] = kNoComp;
        if (hdr.live == 0) freeHeader(store, comp);
    }

    // Promote every live member of the component holding `seed`.  `promote` is
    // called once per live member with its slot index; the component is then
    // retired.  Returns the number of members promoted.
    template <class F>
    uint32_t promoteComponent(OwnerToken owner, uint32_t seed, F&& promote) {
        const uint32_t comp = compOf_[seed];
        if (comp == kNoComp) return 0;
        OwnerStore& store = storeOf(owner);
        if (comp >= store.headers.size() || store.headers[comp].owner != owner)
            return 0;
        Header& hdr = store.headers[comp];
        uint32_t promoted = 0;
        for (uint32_t m = hdr.head; m != kNoComp;) {
            const uint32_t next = next_[m];
            compOf_[m] = kNoComp;   // leaves the LOCAL component system
            prev_[m] = kNoComp;
            next_[m] = kNoComp;
            promote(m);             // caller checks liveness / flips regime
            ++promoted;
            m = next;
        }
        freeHeader(store, comp);
        return promoted;
    }

private:
    struct Header {
        OwnerToken owner = kNoOwnerToken;
        uint32_t head = kNoComp;
        uint32_t tail = kNoComp;
        uint32_t live = 0;
    };

    struct OwnerStore {
        std::vector<Header> headers;
        std::vector<uint32_t> freeList;
    };

    OwnerStore& storeOf(OwnerToken owner) { return ownerStores_[owner.id]; }

    uint32_t newHeader(OwnerStore& store) {
        if (!store.freeList.empty()) {
            const uint32_t id = store.freeList.back();
            store.freeList.pop_back();
            return id;
        }
        store.headers.push_back(Header{});
        return static_cast<uint32_t>(store.headers.size() - 1);
    }

    void freeHeader(OwnerStore& store, uint32_t id) {
        store.headers[id] = Header{};
        store.freeList.push_back(id);
    }

    std::vector<uint32_t> compOf_;
    std::vector<uint32_t> prev_;
    std::vector<uint32_t> next_;
    std::vector<OwnerStore> ownerStores_;
};

}} // namespace loxis::aarc
