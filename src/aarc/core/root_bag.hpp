// AARC per-mutator root multiset as a flat open-addressing table.
//
// M5 added per-mutator root accounting so close() can drop exactly the roots a
// mutator holds and rootRemove can refuse to drop another mutator's root.  The
// first version used std::unordered_map, whose per-insert node allocation made
// every alloc/rootRemove far more expensive than plain ARC's counters and
// knocked the LOCAL fast path off the inline-ARC baseline.  This flat table has
// no per-entry allocation: one vector of (key,count) slots, linear probing,
// uint64 keys = (index << 32) | generation.
//
// Growth accounts for tombstones: heavy alloc/remove churn must not fill the
// table with dead slots and make probing unbounded.

#pragma once

#include <cstdint>
#include <vector>

namespace loxis { namespace aarc {

class RootBag {
public:
    RootBag() = default;

    void add(uint64_t key) {
        if (needsGrow()) grow();
        uint64_t* slot = probeInsert(key);
        const size_t idx = static_cast<size_t>(slot - keys_.data());
        if (*slot == key) {
            ++vals_[idx];
        } else {
            *slot = key;
            vals_[idx] = 1;
            ++size_;
        }
    }

    bool removeOne(uint64_t key) {
        if (keys_.empty()) return false;
        const size_t idx = probeFind(key);
        if (idx == kNotFound) return false;
        if (--vals_[idx] == 0) {
            keys_[idx] = kTombstone;
            ++tombstones_;
            --size_;
        }
        return true;
    }

    bool empty() const { return size_ == 0; }
    size_t capacity() const { return keys_.size(); }
    // Diagnostics.
    uint64_t size() const { return size_; }
    uint64_t tombstones() const { return tombstones_; }

    void reset() {
        keys_.clear();
        vals_.clear();
        size_ = 0;
        tombstones_ = 0;
    }

    template <class F>
    void each(F&& f) const {
        for (size_t i = 0; i < keys_.size(); ++i)
            if (keys_[i] != kEmpty && keys_[i] != kTombstone)
                f(keys_[i], vals_[i]);
    }

private:
    // Smallest table: keeps the probe array inside one or two cache lines for
    // the tiny live sets (a few roots) that dominate real mutators, and makes
    // churn-driven rehashes rare.
    static constexpr uint64_t kMinCapacity = 128;
    static constexpr uint64_t kEmpty = ~0ull;
    static constexpr uint64_t kTombstone = ~0ull - 1ull;
    static constexpr size_t kNotFound = ~size_t(0);

    bool needsGrow() const {
        if (capacity() == 0) return true;
        // Size for the LIVE set only.  Counting tombstones here (the previous
        // rule) let churn inflate the table without bound: a workload that
        // allocates/drops handles keeps `size_` at a handful while tombstones
        // pile up, so the table grew to 512K slots (6MB) and every probe became
        // a cold cache/TLB miss.  Tombstone pressure is handled by rehashing
        // instead, which drops the dead slots and keeps the table small.
        if ((size_ + 1) * 2 >= capacity()) return true;
        // Rehash only once the dead slots are a real fraction of the table: a
        // lower threshold would rehash (two heap allocations) every few ops and
        // cost more than the bloat it avoids.
        return tombstones_ * 2 >= capacity();
    }

    // Rehash live entries into a table sized for them, discarding tombstones.
    void grow() {
        std::vector<uint64_t> oldKeys;
        std::vector<uint32_t> oldVals;
        oldKeys.swap(keys_);
        oldVals.swap(vals_);
        const uint64_t need = (size_ + 1) * 2 + 1;
        keys_.assign(nextPow2(need < kMinCapacity ? kMinCapacity : need), kEmpty);
        vals_.assign(keys_.size(), 0);
        tombstones_ = 0;
        // size_ already counts live entries; re-insert without re-counting.
        for (size_t i = 0; i < oldKeys.size(); ++i) {
            if (oldKeys[i] != kEmpty && oldKeys[i] != kTombstone) {
                uint64_t* slot = probeInsert(oldKeys[i]);
                const size_t idx = static_cast<size_t>(slot - keys_.data());
                *slot = oldKeys[i];
                vals_[idx] = oldVals[i];
            }
        }
    }

    // Returns a pointer to either the matching slot or a free slot (empty or the
    // first tombstone) where the key may be placed.
    uint64_t* probeInsert(uint64_t key) {
        const size_t mask = keys_.size() - 1;
        size_t i = mix(key) & mask;
        size_t firstTomb = kNotFound;
        for (;;) {
            const uint64_t k = keys_[i];
            if (k == key || k == kEmpty) {
                if (k == kEmpty && firstTomb != kNotFound) {
                    // key absent; reclaim the earliest tombstone so the probe
                    // chain stays dense and the load factor holds.
                    --tombstones_;
                    return &keys_[firstTomb];
                }
                return &keys_[i];
            }
            if (k == kTombstone && firstTomb == kNotFound) firstTomb = i;
            i = (i + 1) & mask;
        }
    }

    // Returns the slot holding key, or kNotFound.
    size_t probeFind(uint64_t key) {
        const size_t mask = keys_.size() - 1;
        size_t i = mix(key) & mask;
        for (;;) {
            const uint64_t k = keys_[i];
            if (k == key) return i;
            if (k == kEmpty) return kNotFound;
            i = (i + 1) & mask;
        }
    }

    static size_t mix(uint64_t x) {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }

    static size_t nextPow2(uint64_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    std::vector<uint64_t> keys_;
    std::vector<uint32_t> vals_;
    uint64_t size_ = 0;
    uint64_t tombstones_ = 0;
};

}} // namespace loxis::aarc
