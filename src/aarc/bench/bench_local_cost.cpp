// f=0 cost attribution: is the inline LOCAL path's gap to ARC the reverse-index
// edge maintenance, or the rest of the engine (graph process + per-mutator root
// bookkeeping + pool routing)?
//
// Same benchmark, same workload, three tiers, each run on ARC and on the inline
// AARC engine (Runtime::Mode::Sync == the f=0 LOCAL fast path, one thread):
//   allocOnly  : alloc p,c then rootRemove c,p            (no edges at all)
//   edgeAdd    : + edgeAdd(p,c)                            (one EdgeAdd/pair)
//   churn      : + edgeRemove(p,c) too                     (one EdgeRemove/pair)
//
// Decomposition:
//   allocOnly(ARC) -> allocOnly(AARC)  : cost of graph process + RootBag + pool
//   edgeAdd tier delta                  : cost of ONE intrusive EdgeAdd
//   churn tier delta                    : cost of ONE intrusive EdgeRemove
//
// If allocOnly(AARC) is already well below ARC, the f=0 gap is NOT the reverse
// index — it is the engine's per-op graph bookkeeping.  If allocOnly matches
// but the edge tiers drop AARC, the reverse adjacency is the tax.

#include "../bench/basic_arc.hpp"
#include "arc/runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace loxis::aarc;

namespace {

double nowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum class Tier { AllocOnly, EdgeAdd, Churn };

const char* tierName(Tier t) {
    switch (t) {
        case Tier::AllocOnly: return "allocOnly";
        case Tier::EdgeAdd:   return "edgeAdd";
        case Tier::Churn:     return "churn";
    }
    return "?";
}

// ARC (atomic inline), one thread.
double runArc(int iters, Tier tier) {
    BasicArc arc(1u << 20);
    const double t0 = nowSec();
    for (int i = 0; i < iters; ++i) {
        ObjHandle p = arc.alloc();
        if (p == kNullHandle) continue;
        ObjHandle c = arc.alloc();
        if (c == kNullHandle) { arc.release(p); continue; }
        if (tier != Tier::AllocOnly) arc.addEdge(p, c);
        arc.release(c);
        arc.release(p);
    }
    return nowSec() - t0;
}

// AARC inline (Mode::Sync), one thread.
double runAarc(int iters, Tier tier) {
    Runtime rt(1u << 20, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    const double t0 = nowSec();
    for (int i = 0; i < iters; ++i) {
        ObjHandle p = m->alloc();
        if (p == kNullHandle) continue;
        ObjHandle c = m->alloc();
        if (c == kNullHandle) { m->rootRemove(p); continue; }
        if (tier != Tier::AllocOnly) m->edgeAdd(p, c);
        if (tier == Tier::Churn) m->edgeRemove(p, c);
        m->rootRemove(c);
        m->rootRemove(p);
        if ((i & 0xFFF) == 0) rt.awaitQuiesce();   // reclaim incrementally
    }
    rt.awaitQuiesce();
    return nowSec() - t0;
}

double medianOf(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double runOne(int iters, Tier tier, bool arc, int reps) {
    std::vector<double> secs;
    for (int r = 0; r < reps; ++r)
        secs.push_back(arc ? runArc(iters, tier) : runAarc(iters, tier));
    return medianOf(secs);
}

} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 400000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 7;
    std::printf("bench_local_cost: %d pairs x %d reps (median), T1, Mode::Sync = f=0 inline\n",
                iters, reps);
    std::printf("  %-10s | %12s | %12s | %10s\n", "tier", "ARC obj/s",
                "AARC obj/s", "AARC/ARC");
    for (Tier tier : {Tier::AllocOnly, Tier::EdgeAdd, Tier::Churn}) {
        // ARC counts 2 objects per pair; AARC same.  Both include the pair alloc.
        const double tArc = runOne(iters, tier, true, reps);
        const double tAarc = runOne(iters, tier, false, reps);
        const double arcRate = (2.0 * iters) / tArc;
        const double aarcRate = (2.0 * iters) / tAarc;
        std::printf("  %-10s | %12.0f | %12.0f | %9.1f%%\n", tierName(tier),
                    arcRate, aarcRate, 100.0 * aarcRate / arcRate);
    }
    return 0;
}
