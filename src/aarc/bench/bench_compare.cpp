// Multi-threaded AARC vs ordinary (synchronous atomic) ARC benchmark.
//
// Two workloads, run on the SAME machine / process / thread counts:
//
//   * acyclic : each mutator builds parent->child pairs and releases them.
//               Plain ARC can reclaim these; this compares pure throughput.
//   * cyclic  : 30% of the objects are orphaned self-cycles (obj->obj).
//               AARC collects them (live -> 0); plain ARC cannot and leaks
//               them — reported as live-at-end, not hidden.
//
// All numbers below are measured on this machine; nothing is fabricated or
// scaled.  See bench_alloc.cpp for the ~10 KB/object byte convention.

#include "../bench/basic_arc.hpp"
#include "arc/runtime.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

struct Result {
    double seconds = 0;
    long long objects = 0;
    unsigned liveAtEnd = 0;
    unsigned long long errors = 0;
    // AARC worker/producer diagnostics (0 for the plain-ARC baseline)
    unsigned long long msgs = 0;
    unsigned long long spins = 0;
    unsigned long long rounds = 0;
    unsigned long long peakOcc = 0;
    unsigned long long reclaimed = 0;
};

constexpr uint32_t kPoolCap = 1u << 20;

// ---------------------------------------------------------------------------
// thread scaffolding: run `job(threadIndex)` on `threads` threads and join.
// ---------------------------------------------------------------------------
template <class Job>
double runThreads(int threads, Job&& job) {
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&job, t] { job(t); });
    for (auto& t : ts) t.join();
    return 0;   // caller times the whole block
}

// ---- acyclic workload ------------------------------------------------------

Result acyclic_aarc(int threads, int iters) {
    Runtime rt(kPoolCap, Runtime::Mode::Async);
    std::atomic<long long> objs{0};
    const auto t0 = std::chrono::steady_clock::now();
    runThreads(threads, [&](int) {
        auto m = rt.makeMutator();
        for (int i = 0; i < iters; ++i) {
            ObjHandle p = m->alloc();
            if (p == kNullHandle) continue;
            ObjHandle c = m->alloc();
            if (c == kNullHandle) { m->rootRemove(p); continue; }
            m->edgeAdd(p, c);
            m->rootRemove(c);
            m->rootRemove(p);
            objs.fetch_add(2, std::memory_order_relaxed);
        }
    });
    rt.awaitQuiesce();
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    Result r;
    r.seconds = s;
    r.objects = objs.load();
    r.liveAtEnd = rt.liveCells();
    r.errors = rt.engine().totalErrors();
    r.msgs = rt.engine().msgsProcessed();
    r.spins = rt.spinCount();
    r.rounds = rt.roundsDone();
    r.peakOcc = rt.peakQueueOccupancy();
    r.reclaimed = rt.engine().cellsReclaimedZero() + rt.engine().cellsReclaimedCycle();
    return r;
}

Result acyclic_arc(int threads, int iters) {
    BasicArc arc(kPoolCap);
    std::atomic<long long> objs{0};
    const auto t0 = std::chrono::steady_clock::now();
    runThreads(threads, [&](int) {
        for (int i = 0; i < iters; ++i) {
            ObjHandle p = arc.alloc();
            if (p == kNullHandle) continue;
            ObjHandle c = arc.alloc();
            if (c == kNullHandle) { arc.release(p); continue; }
            arc.addEdge(p, c);
            arc.release(c);
            arc.release(p);
            objs.fetch_add(2, std::memory_order_relaxed);
        }
    });
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    Result r;
    r.seconds = s;
    r.objects = objs.load();
    r.liveAtEnd = arc.liveCells();
    r.errors = arc.errorCount();
    return r;
}

// ---- cyclic workload (30% orphaned self-cycles) ---------------------------

Result cyclic_aarc(int threads, int iters) {
    Runtime rt(kPoolCap, Runtime::Mode::Async);
    std::atomic<long long> objs{0};
    const auto t0 = std::chrono::steady_clock::now();
    runThreads(threads, [&](int) {
        auto m = rt.makeMutator();
        for (int i = 0; i < iters; ++i) {
            if ((i % 10) < 3) {             // 30% orphaned self-cycle
                ObjHandle a = m->alloc();
                if (a == kNullHandle) continue;
                m->edgeAdd(a, a);
                m->rootRemove(a);
                objs.fetch_add(1, std::memory_order_relaxed);
            } else {                        // 70% acyclic
                ObjHandle p = m->alloc();
                if (p == kNullHandle) continue;
                ObjHandle c = m->alloc();
                if (c == kNullHandle) { m->rootRemove(p); continue; }
                m->edgeAdd(p, c);
                m->rootRemove(c);
                m->rootRemove(p);
                objs.fetch_add(2, std::memory_order_relaxed);
            }
        }
    });
    rt.awaitQuiesce();
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    Result r;
    r.seconds = s;
    r.objects = objs.load();
    r.liveAtEnd = rt.liveCells();
    r.errors = rt.engine().totalErrors();
    r.msgs = rt.engine().msgsProcessed();
    r.spins = rt.spinCount();
    r.rounds = rt.roundsDone();
    r.peakOcc = rt.peakQueueOccupancy();
    r.reclaimed = rt.engine().cellsReclaimedZero() + rt.engine().cellsReclaimedCycle();
    return r;
}

Result cyclic_arc(int threads, int iters) {
    BasicArc arc(kPoolCap);
    std::atomic<long long> objs{0};
    const auto t0 = std::chrono::steady_clock::now();
    runThreads(threads, [&](int) {
        for (int i = 0; i < iters; ++i) {
            if ((i % 10) < 3) {             // 30% self-cycle -> NEVER reclaimed
                ObjHandle a = arc.alloc();
                if (a == kNullHandle) continue;
                arc.addEdge(a, a);
                arc.release(a);
                objs.fetch_add(1, std::memory_order_relaxed);
            } else {
                ObjHandle p = arc.alloc();
                if (p == kNullHandle) continue;
                ObjHandle c = arc.alloc();
                if (c == kNullHandle) { arc.release(p); continue; }
                arc.addEdge(p, c);
                arc.release(c);
                arc.release(p);
                objs.fetch_add(2, std::memory_order_relaxed);
            }
        }
    });
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    Result r;
    r.seconds = s;
    r.objects = objs.load();
    r.liveAtEnd = arc.liveCells();
    r.errors = arc.errorCount();
    return r;
}

void printRow(const char* engine, const char* wl, int threads, const Result& r,
              long long expectedLeak) {
    const double objPerSec = static_cast<double>(r.objects) / r.seconds;
    std::printf("  %-6s %-8s T=%-2d  %8.0f obj/s   %6.2f ms   live@end=%6u  errs=%llu\n",
                engine, wl, threads, objPerSec, r.seconds * 1e3,
                r.liveAtEnd, (unsigned long long)r.errors);
    if (expectedLeak >= 0)
        std::printf("           (expected live-at-end: %lld)\n", expectedLeak);
    if (r.msgs)
        std::printf("           AARC diag: msgs=%llu  spins=%llu  rounds=%llu  "
                    "peakQ=%llu  reclaimed=%llu\n",
                    r.msgs, r.spins, r.rounds, r.peakOcc, r.reclaimed);
}

} // namespace

int main(int argc, char** argv) {
    const int threads = argc > 1 ? std::atoi(argv[1]) : 8;
    const int iters = argc > 2 ? std::atoi(argv[2]) : 30000;
    const long long cyclicCount =
        static_cast<long long>(threads) * iters * 3 / 10;   // leaked self-cycles

    std::printf("bench_compare: threads=%d  iters/thread=%d  pool=%u cells\n\n",
                threads, iters, kPoolCap);

    std::printf("-- acyclic (plain ARC can reclaim everything) --\n");
    {
        Result a = acyclic_arc(threads, iters);
        printRow("ARC", "acyclic", threads, a, 0);
        Result b = acyclic_aarc(threads, iters);
        printRow("AARC", "acyclic", threads, b, 0);
    }

    std::printf("\n-- cyclic 30%% (ordinary ARC leaks self-cycles) --\n");
    {
        Result a = cyclic_arc(threads, iters);
        printRow("ARC", "cyclic", threads, a, cyclicCount);
        Result b = cyclic_aarc(threads, iters);
        printRow("AARC", "cyclic", threads, b, 0);
    }

    std::printf("\nnote: AARC async worker stays single-threaded (no multi-worker\n"
                "      scheduler yet); plain ARC scales per-thread. The point of the\n"
                "      cyclic row is correctness (AARC live=0) vs leak, not speed.\n");
    return 0;
}
