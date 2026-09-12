// M11: producer hot-path attribution.
//
// The async producer path is `pool alloc -> RootBag -> stage message -> flush`,
// ~180ns/pair at batch=32.  This bench runs the SAME workload as
// bench_async_transport and splits that cost with cycle-level probes
// (AARC_PROFILE build).  Per-op phases are sampled 1/64 so the rdtsc pairs
// themselves (~10ns each) do not dwarf what is being measured; the flush body
// is timed on every flush (one per batch) and its backpressure spin is split
// out separately.
//
// Attribution:
//   poolAlloc : CellPool::allocOwned (recycle pile pop or sharded free list)
//   rootBag   : RootBag add / removeOne
//   stage     : publish() body (publication window + batch write), no flush
//   flush     : flushSlot() body minus spin (fold pass + ring pushes + wake)
//   spin      : waiting for ring room (backpressure -- worker time, not ours)
//   other     : wall-clock total minus the above
//
// Usage: bench_producer_cost [iters] [reps] [batch] [fold]

#include "arc/runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

double nowSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if AARC_PROFILE
// Cycles burned by one ProfTsc (two rdtsc) -- measured once, subtracted from
// every sampled phase before the 1/64 scale-up.
double overheadCycles() {
    const int kN = 20000;
    uint64_t acc = 0;
    for (int i = 0; i < kN; ++i) {
        ProfTsc t(true);
        acc += t.stop();
    }
    return double(acc) / kN;
}

double tscHz() {
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    while (std::chrono::duration<double>(
               std::chrono::steady_clock::now() - c0).count() < 0.05) {}
    const uint64_t t1 = __rdtsc();
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - c0).count();
    return double(t1 - t0) / s;
}
#endif

struct Cell {
    double nsPerPair = 0;
    double poolAlloc = 0, rootBag = 0, stage = 0, flush = 0, spin = 0;
    double prologue = 0, other = 0;
    uint64_t live = 0, errs = 0, flushes = 0;
};

Cell run(int iters, uint32_t batch, bool fold, double cycPerNs, bool spanPath,
         bool fastRootValid = false) {
    Runtime rt(1u << 20, Runtime::Mode::Async, true, batch, fold, spanPath);
    rt.setFastRootValid(fastRootValid);
    auto m = rt.makeMutator();
    const double t0 = nowSec();
#if AARC_PROFILE
    const uint64_t c0 = __rdtsc();
#endif
    for (int i = 0; i < iters; ++i) {
        ObjHandle p = m->alloc();
        if (p == kNullHandle) continue;
        ObjHandle c = m->alloc();
        if (c == kNullHandle) { m->rootRemove(p); continue; }
        m->edgeAdd(p, c);
        m->rootRemove(c);
        m->rootRemove(p);
    }
    rt.awaitQuiesce();
    const double s = nowSec() - t0;
#if AARC_PROFILE
    const uint64_t c1 = __rdtsc();
    std::printf("    [tsc] loop %.2f GHz (calibrated %.2f GHz)\n",
                double(c1 - c0) / s / 1e9, cycPerNs / 1e9);
#endif

    Cell cell;
    cell.nsPerPair = s * 1e9 / double(iters);
    std::printf("    [raw] pool=%llu bag=%llu ops=%llu stage=%llu flush=%llu spin=%llu\n",
                (unsigned long long)m->prof().poolAlloc,
                (unsigned long long)m->prof().rootBag,
                (unsigned long long)m->prof().ops,
                (unsigned long long)rt.slotProf().stage,
                (unsigned long long)rt.slotProf().flush,
                (unsigned long long)rt.slotProf().spin);
#if AARC_PROFILE
    const double nsPerCycle = 1e9 / cycPerNs;   // ns per TSC cycle
    // Sampled phases: `cycles` cover 1/64 of the ops of that kind, and each
    // sampled op carries the timer's own cost (2 rdtsc).  Subtract the measured
    // overhead per sample BEFORE scaling by 64, else it is multiplied by 64 too
    // and swamps the attribution.
    const double oh = overheadCycles();          // cycles per ProfTsc pair
    // `cycles` covers `samples` sampled ops; scaling back to all ops is *64.
    auto phaseNs = [&](uint64_t cycles, uint64_t samples) {
        const double net = double(cycles) - double(samples) * oh;
        return net * 64.0 * nsPerCycle / iters;
    };
    const uint64_t poolOps = m->prof().poolOps;
    const uint64_t bagOps = m->prof().bagOps;
    const Runtime::SlotProf sp = rt.slotProf();
    cell.poolAlloc = phaseNs(m->prof().poolAlloc, poolOps / 64);
    cell.rootBag = phaseNs(m->prof().rootBag, bagOps / 64);
    cell.stage = phaseNs(sp.stage, sp.stageOps / 64);
    cell.prologue = phaseNs(m->prof().prologue, m->prof().preOps / 64);
    // Flush is timed on every flush (not sampled): subtract one overhead each.
    cell.flushes = sp.flushes;
    const double flushNet = double(sp.flush) - double(sp.flushes) * oh;
    cell.spin = double(sp.spin) * nsPerCycle / iters;
    cell.flush = flushNet * nsPerCycle / iters - cell.spin;
    cell.other = cell.nsPerPair - cell.poolAlloc - cell.rootBag - cell.stage -
                 cell.flush - cell.spin - cell.prologue;
#endif
    std::printf("    [bag] cap=%zu size=%llu tomb=%llu\n", m->roots().capacity(),
                (unsigned long long)m->roots().size(),
                (unsigned long long)m->roots().tombstones());
    cell.live = rt.liveCells();
    cell.errs = rt.engine().totalErrors();
    return cell;
}

} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 200000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 7;
    const uint32_t batch = argc > 3 ? uint32_t(std::atoi(argv[3])) : 32;
    const bool fold = argc > 4 ? std::atoi(argv[4]) != 0 : false;

#if AARC_PROFILE
    const double hz = tscHz();
    std::printf("bench_producer_cost: %d pairs x %d reps, batch=%u fold=%d, T=1\n",
                iters, reps, batch, int(fold));
    std::printf("TSC %.2f GHz, phases sampled 1/64\n\n", hz / 1e9);

    // Interleaved A/B: zero-copy span vs staged batch + per-message push.
    // Alternate the order every rep: a fixed order lets any within-rep drift
    // (warm-up, frequency ramp) masquerade as a difference between variants.
    std::vector<double> spanNs, pushNs;
    for (int r = 0; r < reps; ++r) {
        if (r & 1) {
            pushNs.push_back(run(iters, batch, fold, hz, false).nsPerPair);
            spanNs.push_back(run(iters, batch, fold, hz, true).nsPerPair);
        } else {
            spanNs.push_back(run(iters, batch, fold, hz, true).nsPerPair);
            pushNs.push_back(run(iters, batch, fold, hz, false).nsPerPair);
        }
    }
    std::sort(spanNs.begin(), spanNs.end());
    std::sort(pushNs.begin(), pushNs.end());
    std::printf("  A/B span vs push (median ns/pair): span=%.2f push=%.2f  (%.3fx)\n",
                spanNs[reps / 2], pushNs[reps / 2],
                spanNs[reps / 2] ? pushNs[reps / 2] / spanNs[reps / 2] : 0.0);

    // M12b: rootRemove fast path (RootBag implication, no pool validity probe).
    std::vector<double> slowValid, fastValid;
    for (int r = 0; r < reps; ++r) {
        if (r & 1) {
            fastValid.push_back(run(iters, batch, fold, hz, true, true).nsPerPair);
            slowValid.push_back(run(iters, batch, fold, hz, true, false).nsPerPair);
        } else {
            slowValid.push_back(run(iters, batch, fold, hz, true, false).nsPerPair);
            fastValid.push_back(run(iters, batch, fold, hz, true, true).nsPerPair);
        }
    }
    std::sort(slowValid.begin(), slowValid.end());
    std::sort(fastValid.begin(), fastValid.end());
    std::printf("  A/B rootRemove valid-probe vs bag-implication: slow=%.2f fast=%.2f (%.3fx)\n\n",
                slowValid[reps / 2], fastValid[reps / 2],
                fastValid[reps / 2] ? slowValid[reps / 2] / fastValid[reps / 2] : 0.0);

    std::vector<Cell> cells;
    for (int r = 0; r < reps; ++r) cells.push_back(run(iters, batch, fold, hz, true));
    // Report the rep whose TOTAL is the median: taking the median of each phase
    // independently can mix reps from different machine states, which makes the
    // phases sum to more than the wall clock.
    std::vector<double> totals;
    for (const Cell& c : cells) totals.push_back(c.nsPerPair);
    std::sort(totals.begin(), totals.end());
    const double med = totals[reps / 2];
    const Cell* mid = &cells[0];
    for (const Cell& c : cells)
        if (std::abs(c.nsPerPair - med) < std::abs(mid->nsPerPair - med)) mid = &c;

    std::printf("  %-10s %10s %8s\n", "phase", "ns/pair", "share");
    const struct { const char* name; double ns; } rows[] = {
        {"poolAlloc", mid->poolAlloc}, {"rootBag", mid->rootBag},
        {"stage", mid->stage},         {"flush", mid->flush},
        {"spin", mid->spin},           {"prologue", mid->prologue},
        {"other", mid->other},
    };
    for (const auto& row : rows)
        std::printf("  %-10s %10.2f %7.1f%%\n", row.name, row.ns,
                    med > 0 ? 100.0 * row.ns / med : 0.0);
    std::printf("  %-10s %10.2f  (median of %d; live=%llu errs=%llu)\n\n",
                "TOTAL", med, reps, (unsigned long long)mid->live,
                (unsigned long long)mid->errs);
#else
    std::printf("bench_producer_cost: built without AARC_PROFILE -- no attribution.\n");
#endif
    return 0;
}
