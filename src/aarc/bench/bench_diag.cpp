// ONE-SHOT locating diagnostic for the T8 Adaptive-LOCAL gap (12M vs ARC 15M).
// Answers three binary questions on a pure thread-local acyclic workload at
// T=8 — it does NOT grind throughput numbers beyond one battery.
//
//   V1  spawnWorker=true,  collect cadence 2048   (what bench_adaptive ran)
//   V2  spawnWorker=false, collect cadence 2048   Q1: is the idle worker guilty?
//   V3  spawnWorker=true,  collect cadence 256    Q3: is collect scheduling the
//                                                  tax (8x more frequent sweeps)?
//
// Built twice from this file:
//   * bench_diag   — throughput variants (AARC_INSTRUMENT off)
//   * bench_diag_i — same battery + traversal counters (AARC_INSTRUMENT=1) to
//                    answer Q2: does a LOCAL engine visit the whole pool, and
//                    does visited/allocated grow with T?  Acyclic => no cycle
//                    pass should ever run, and zeroVisited should be ~2 per
//                    tree (just the garbage it frees), NOT O(pool) per sweep.

#include "arc/runtime.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

constexpr uint32_t kPoolCap = 1u << 20;

struct Out {
    double objPerSec = 0;
    unsigned live = 0;
    unsigned long long errs = 0;
    unsigned long long promotions = 0;
    // instrument counters, summed over the per-owner local engines
    unsigned long long sweeps = 0;
    unsigned long long zeroVisited = 0;
    unsigned long long cyclePasses = 0;
    unsigned long long tarjans = 0;
};

Out run(int threads, int iters, bool spawnWorker, int cadence) {
    Runtime rt(kPoolCap, Runtime::Mode::Adaptive, spawnWorker);
    std::vector<std::unique_ptr<Mutator>> ms(threads);
    std::atomic<long long> objs{0};
    const auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            ms[t] = rt.makeMutator();
            auto& m = ms[t];
            for (int i = 0; i < iters; ++i) {
                ObjHandle p = m->allocLocal();
                if (p == kNullHandle) continue;
                ObjHandle c = m->allocLocal();
                if (c == kNullHandle) { m->rootRemove(p); continue; }
                m->edgeAdd(p, c);
                m->rootRemove(c);
                m->rootRemove(p);
                objs.fetch_add(2, std::memory_order_relaxed);
                if ((i & (cadence - 1)) == 0) m->collectLocal();
            }
        });
    }
    for (auto& t : ts) t.join();
    for (auto& m : ms) m->collectLocal();   // sweep each owner's tail
    if (spawnWorker) rt.awaitQuiesce();     // no worker -> nothing to quiesce

    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    Out o;
    o.objPerSec = static_cast<double>(objs.load()) / s;
    o.live = rt.liveCells();
    o.promotions = rt.promotionsDone();
    for (auto& m : ms) {
        AarcEngine* e = m->localEngine();
        o.errs += e->totalErrors();
        o.sweeps += e->instrumentSweeps();
        o.zeroVisited += e->instrumentZeroVisited();
        o.cyclePasses += e->instrumentCyclePasses();
        o.tarjans += e->instrumentTarjans();
    }
    o.errs += rt.engine().totalErrors();
    return o;
}

void printRow(const char* name, bool worker, int cadence, const Out& o,
              long long objs) {
    std::printf("  %-22s worker=%d cadence=%-5d %9.0f obj/s  live=%4u errs=%llu"
                " promotions=%llu\n",
                name, int(worker), cadence, o.objPerSec, o.live,
                (unsigned long long)o.errs, o.promotions);
#if AARC_INSTRUMENT
    std::printf("      counter: sweeps=%llu zeroVisited=%llu cyclePasses=%llu"
                " tarjans=%llu   zeroVisited/objects=%.3f\n",
                o.sweeps, o.zeroVisited, o.cyclePasses, o.tarjans,
                objs ? static_cast<double>(o.zeroVisited) / static_cast<double>(objs) : 0.0);
#endif
}

} // namespace

int main(int argc, char** argv) {
    const int threads = 8;
    const int iters = argc > 1 ? std::atoi(argv[1]) : 150000;
    const long long objs = static_cast<long long>(threads) * iters * 2;

    std::printf("bench_diag: T=%d, %d trees/thread (%lld objs), pool=%u, "
                "AARC_INSTRUMENT=%d\n\n",
                threads, iters, objs, kPoolCap, int(AARC_INSTRUMENT));

    {
        Out o = run(threads, iters, true, 2048);
        printRow("V1 worker+cad2048", true, 2048, o, objs);
    }
    {
        Out o = run(threads, iters, false, 2048);
        printRow("V2 no-worker+cad2048", false, 2048, o, objs);
    }
    {
        Out o = run(threads, iters, true, 256);
        printRow("V3 worker+cad256", true, 256, o, objs);
    }

    std::printf("\nQ1 idle-worker guilt   : V1 vs V2 (worker off should match/beat V1)\n"
                "Q3 collect-scheduling tax: V1 vs V3 (8x cadence: if flat, sweep isn't the tax)\n"
                "Q2 whole-pool scan      : zeroVisited/objects should be ~1.0 and NOT grow\n"
                "                           with T if the LOCAL engine only visits its own garbage;\n"
                "                           cyclePasses/tarjans must be 0 on an acyclic workload.\n");
    return 0;
}
