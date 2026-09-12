// M6 A/B: promotion closure complexity.
//
// Question: what does reverse adjacency buy, and at what edge-maintenance cost?
//
// Axis 1 — heap size (component fixed): a growing live heap must not change the
// cost of promoting a fixed component.  Legacy closure scans the live heap per
// hand-off (forEachLive); reverse-index closure walks only the component.
//
// Axis 2 — component size (heap fixed): reverse cost should grow with the
// escaped component, not with the heap.
//
// Axis 3 — edge maintenance: EdgeAdd/EdgeRemove throughput of the intrusive
// reverse-index RefTable vs a bare outgoing-vector representation (the pre-M6
// structure), i.e. what we pay to keep the incoming lists.
//
// Methodology (per the machine caveat): the measured promotion is a
// synchronous self-promotion (owner crosses LOCAL->SHARED via a sentinel edge)
// so there is no cross-thread handshake in the timed region.  Each cell is the
// median of reps with the min..max band printed; compile this file once without
// and once with AARC_LEGACY_POOL_CLOSURE for the legacy closure path.
//
// Usage: bench_promotion_scaling  [liveHeap for axis1] [componentSizes] [reps]

#include "../bench/basic_arc.hpp"
#include "arc/runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

#include <cstdio>
#include <cstdlib>

double nowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Median of a small sample.
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Build a rooted chain of `size` LOCAL cells owned by `m`; returns the handles
// so the caller can rootRemove them after promotion.
std::vector<ObjHandle> buildChain(Mutator* m, uint32_t size) {
    std::vector<ObjHandle> cells;
    cells.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        ObjHandle h = m->allocLocal();
        if (h == kNullHandle) break;
        if (!cells.empty()) m->edgeAdd(cells.back(), h);   // k -> k+1
        cells.push_back(h);
    }
    return cells;
}

// Promote the whole chain synchronously by adding a LOCAL->SHARED edge from its
// head; this self-promotes the chain's component on this thread.
// Returns microseconds for the single edgeAdd call.
double promoteSelf(Mutator* m, ObjHandle head, ObjHandle sentinel) {
    const double t0 = nowSec();
    m->edgeAdd(head, sentinel);
    const double dt = nowSec() - t0;
    return dt * 1e6;
}

// --- Axis 1 + 2 -----------------------------------------------------------

// Pre-alloc `live` LOCAL cells (rooted, never freed) so the live heap matches
// `live`.  Returns a runtime whose heap is dominated by those fillers.
// The runtime's pool capacity is sized for `live` + the largest component.
struct Heap {
    std::unique_ptr<Runtime> rt;
    std::unique_ptr<Mutator> m;
    uint32_t live = 0;
};

void fillLive(Heap& heap, uint32_t live, uint32_t maxComponent) {
    const uint32_t cap = live + maxComponent + (1u << 14);
    heap.rt = std::make_unique<Runtime>(cap, Runtime::Mode::Adaptive);
    heap.m = heap.rt->makeMutator();
    for (uint32_t i = 0; i < live; ++i) {
        if (heap.m->allocLocal() == kNullHandle) break;
        ++heap.live;
    }
}

// One measurement: promote `componentSize` LOCAL nodes (chain) against a live
// heap of `live` cells, `reps` times.  Returns median per-promotion us.
std::vector<double> promoteAt(Heap& heap, uint32_t componentSize, int reps) {
    ObjHandle sentinel = heap.m->allocShared();   // reusable SHARED target
    if (sentinel == kNullHandle) return {};
    std::vector<double> us;
    for (int r = 0; r < reps; ++r) {
        std::vector<ObjHandle> chain = buildChain(heap.m.get(), componentSize);
        if (chain.empty()) break;
        us.push_back(promoteSelf(heap.m.get(), chain[0], sentinel));
        for (ObjHandle h : chain) heap.m->rootRemove(h);   // now SHARED -> worker
        heap.rt->awaitQuiesce();                            // reclaim the chain
    }
    heap.m->rootRemove(sentinel);
    return us;
}

void axis_heapSize() {
    constexpr uint32_t kComponent = 100;
    constexpr uint32_t kHeaps[] = {10 * 1000, 100 * 1000, 500 * 1000, 1000 * 1000};
    std::printf("\n-- Axis 1: fixed component=%u, growing live heap --\n",
                kComponent);
    std::printf("  live heap   per-promotion us (median, reps=%d)\n", 7);
    for (uint32_t live : kHeaps) {
        Heap heap;
        const double f0 = nowSec();
        fillLive(heap, live, kComponent);
        const double f1 = nowSec();
        std::printf("    [heap %uK: filled %u in %.2fs, promoting...]\n",
                    live / 1000, heap.live, f1 - f0);
        std::vector<double> us = promoteAt(heap, kComponent, 7);
        std::printf("    [heap %uK: done]\n", live / 1000);
        heap.m.reset();
        heap.rt.reset();
        if (us.empty()) {
            std::printf("  %8uK    (could not build)\n", live / 1000);
            continue;
        }
        std::printf("  %8uK   %9.2f   [%8.2f .. %8.2f]\n", live / 1000,
                    median(us),
                    *std::min_element(us.begin(), us.end()),
                    *std::max_element(us.begin(), us.end()));
    }
}

void axis_componentSize() {
    constexpr uint32_t kLive = 300 * 1000;
    constexpr uint32_t kComponents[] = {1, 10, 100, 1000, 10000};
    std::printf("\n-- Axis 2: fixed live heap=%uK, growing component --\n",
                kLive / 1000);
    std::printf("  component   per-promotion us (median, reps=%d)\n", 7);
    Heap heap;
    fillLive(heap, kLive, 20000);
    for (uint32_t cs : kComponents) {
        std::vector<double> us = promoteAt(heap, cs, 7);
        if (us.empty()) {
            std::printf("  %7u    (could not build)\n", cs);
            continue;
        }
        std::printf("  %7u   %9.2f   [%8.2f .. %8.2f]\n", cs, median(us),
                    *std::min_element(us.begin(), us.end()),
                    *std::max_element(us.begin(), us.end()));
    }
    heap.m.reset();
    heap.rt.reset();
}

// --- Axis 3: edge-maintenance cost -----------------------------------------

// Bare outgoing-vector representation (the pre-M6 structure): per-slot vector
// of handles plus two signed counters.
// Adapter over the intrusive RefTable so the representation bench is uniform.
struct RevTable {
    explicit RevTable(uint32_t cap) : table(cap) {}
    void add(ObjHandle src, ObjHandle dst) { table.addOut(src, dst); }
    bool remove(ObjHandle src, ObjHandle dst) { return table.removeOut(src, dst); }
    RefTable table;
};

struct LegacyEdges {
    explicit LegacyEdges(uint32_t cap)
        : edge(cap, 0), outs(cap) {}
    void add(ObjHandle src, ObjHandle dst) {
        ++edge[dst.index];
        outs[src.index].push_back(dst);
    }
    bool remove(ObjHandle src, ObjHandle dst) {
        auto& v = outs[src.index];
        for (size_t k = 0; k < v.size(); ++k)
            if (v[k] == dst) {
                v.erase(v.begin() + static_cast<ptrdiff_t>(k));
                --edge[dst.index];
                return true;
            }
        return false;
    }
    std::vector<int64_t> edge;
    std::vector<std::vector<ObjHandle>> outs;
};

struct EdgeRates { double add; double remove; };

template <class Table>
EdgeRates benchEdgeOps(int ops) {
    std::vector<ObjHandle> pool;
    pool.reserve(static_cast<size_t>(ops));
    for (int i = 0; i < ops; ++i)
        pool.push_back(ObjHandle{static_cast<uint32_t>(i), 0});
    Table table(static_cast<uint32_t>(ops + 1));

    const double a0 = nowSec();
    for (int i = 0; i < ops; ++i)
        table.add(pool[static_cast<size_t>(i)],
                  pool[static_cast<size_t>((i + 1) % ops)]);
    const double a1 = nowSec();

    const double r0 = nowSec();
    for (int i = 0; i < ops; ++i)
        table.remove(pool[static_cast<size_t>(i)],
                     pool[static_cast<size_t>((i + 1) % ops)]);
    const double r1 = nowSec();

    return {ops / (a1 - a0), ops / (r1 - r0)};
}

void axis_edgeCost() {
    constexpr int kOps = 500000;
    std::printf("\n-- Axis 3: edge maintenance (%d edges on a %d-slot table) --\n",
                kOps, kOps);
    std::printf("  %-16s %12s %12s\n", "", "EdgeAdd/s", "EdgeRemove/s");
    const EdgeRates legacy = benchEdgeOps<LegacyEdges>(kOps);
    const EdgeRates reverse = benchEdgeOps<RevTable>(kOps);
    std::printf("  %-16s %12.0f %12.0f\n", "legacy vector", legacy.add,
                legacy.remove);
    std::printf("  %-16s %12.0f %12.0f\n", "reverse index", reverse.add,
                reverse.remove);
    std::printf("\n  per-edge metadata:\n");
    std::printf("    legacy : ObjHandle in std::vector -> ~8 B/edge + vector slack\n");
    std::printf("    reverse: EdgeRec{src,dst,prevOut,nextOut,prevIn,nextIn,nextFree,live}\n");
    std::printf("             = 40 B/edge (2x8 handles + 4x4 links + 4 nextFree + pad);\n");
    std::printf("    per-slot Rec gains outHead/inHead/outCount/inCount (+16 B)\n");
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int axis = argc > 1 ? std::atoi(argv[1]) : 0;
#ifdef AARC_LEGACY_POOL_CLOSURE
    std::printf("bench_promotion_scaling [LEGACY O(pool) closure]\n");
#else
    std::printf("bench_promotion_scaling [reverse-index closure]\n");
#endif
    if (axis == 0 || axis == 1) axis_heapSize();
    if (axis == 0 || axis == 2) axis_componentSize();
    if (axis == 0 || axis == 3) axis_edgeCost();
    return 0;
}
