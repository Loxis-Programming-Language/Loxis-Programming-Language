// M4: does the adaptive split fix the async tax?  (design doc §7 M4)
//
// Measured premise (bench_local_fastpath): AARC-sync (the same engine, inline)
// already beats atomic ARC, so the entire multi-x gap of AARC-async is queue +
// worker round-trip.  Adaptive should keep objects LOCAL (inline) until they
// actually cross a thread, so a thread-local workload must track ARC — NOT the
// async worker's ~4-5M collapse.
//
// What this runs on the same machine / process / thread count:
//   * ARC            : inline synchronous atomic RC      (baseline)
//   * AARC-async     : uniform async queue + worker     (the old tax)
//   * Adaptive-LOCAL : pure thread-local acyclic work; every cell stays LOCAL,
//                      reclaimed by each owner's inline engine at collectLocal().
//                      Zero escapes -> zero promotions.  The M4 prediction: this
//                      tracks ARC (and scales with threads), not async.
//   * Adaptive 30%-escape: every ~3rd root is handed to a second thread, so that
//                      tree's LOCAL component is promoted to the worker.  This
//                      row is a CORRECTNESS gate (live=0, errs=0) plus an honest
//                      throughput number — promotion currently builds a reverse
//                      adjacency by scanning the whole pool per hand-off, so it
//                      dominates the cost (see the note at the bottom).
//
// No number is fabricated or scaled; config is the current Release build
// (AARC_ENABLE_STATS on, no LTO/PGO on this MinGW).  ~10 KB/object byte
// convention is irrelevant here (we count objects).

#include "../bench/basic_arc.hpp"
#include "arc/runtime.hpp"
#include <algorithm>
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
    unsigned long long msgs = 0;      // engine messages processed (AARC rows)
    unsigned long long spins = 0;
    unsigned long long rounds = 0;
    unsigned long long peakOcc = 0;
    unsigned long long reclaimed = 0;
    unsigned long long promotions = 0; // adaptive only
};

constexpr uint32_t kPoolCap = 1u << 20;

template <class Job>
void runThreads(int threads, Job&& job) {
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&job, t] { job(t); });
    for (auto& t : ts) t.join();
}

// ---- acyclic family ---------------------------------------------------------

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

Result acyclic_async(int threads, int iters) {
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

Result acyclic_adaptive_local(int threads, int iters) {
    Runtime rt(kPoolCap, Runtime::Mode::Adaptive);
    std::vector<std::unique_ptr<Mutator>> ms(threads);
    std::atomic<long long> objs{0};
    const auto t0 = std::chrono::steady_clock::now();
    runThreads(threads, [&](int t) {
        ms[t] = rt.makeMutator();
        auto& m = ms[t];
        for (int i = 0; i < iters; ++i) {
            ObjHandle p = m->allocLocal();          // stays LOCAL, inline engine
            if (p == kNullHandle) continue;
            ObjHandle c = m->allocLocal();
            if (c == kNullHandle) { m->rootRemove(p); continue; }
            m->edgeAdd(p, c);
            m->rootRemove(c);
            m->rootRemove(p);
            objs.fetch_add(2, std::memory_order_relaxed);
            if ((i & 0x7FF) == 0) m->collectLocal();   // reclaim this owner's zeros
        }
    });
    for (auto& m : ms) m->collectLocal();           // sweep the tail on each owner
    rt.awaitQuiesce();
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    Result r;
    r.seconds = s;
    r.objects = objs.load();
    r.liveAtEnd = rt.liveCells();
    for (auto& m : ms) r.errors += m->localEngine()->totalErrors();
    r.errors += rt.engine().totalErrors();
    r.promotions = rt.promotionsDone();
    return r;
}

// ---- 30% cross-thread escape (correctness gate + honest throughput) --------

Result escape_30(int totalTrees) {
    Runtime rt(1u << 14, Runtime::Mode::Adaptive);
    std::vector<std::atomic<bool>> ready(totalTrees);
    std::vector<std::atomic<bool>> ack(totalTrees);
    std::vector<ObjHandle> pub(totalTrees);
    for (int i = 0; i < totalTrees; ++i) {
        ready[i].store(false, std::memory_order_relaxed);
        ack[i].store(false, std::memory_order_relaxed);
    }
    std::atomic<int> escFail{0};
    std::atomic<long long> objs{0};
    std::unique_ptr<Mutator> mOwner, mEsc;

    const auto t0 = std::chrono::steady_clock::now();
    std::thread owner([&] {
        mOwner = rt.makeMutator();
        for (int i = 0; i < totalTrees; ++i) {
            ObjHandle p = mOwner->allocLocal();
            if (p == kNullHandle) continue;
            ObjHandle c = mOwner->allocLocal();
            if (c == kNullHandle) { mOwner->rootRemove(p); continue; }
            mOwner->edgeAdd(p, c);
            objs.fetch_add(2, std::memory_order_relaxed);
            if (i % 10 < 3) {                    // 30% escape to the other thread
                pub[i] = p;
                ready[i].store(true, std::memory_order_release);
                while (!ack[i].load(std::memory_order_acquire))
                    mOwner->pollPromotions();    // serve the promotion request
                mOwner->rootRemove(p);           // now SHARED -> worker drops it
                mOwner->rootRemove(c);
            } else {
                mOwner->rootRemove(p);
                mOwner->rootRemove(c);
                if ((i & 0x3FF) == 0) mOwner->collectLocal();
            }
        }
    });

    std::thread escaper([&] {
        mEsc = rt.makeMutator();
        for (int i = 0; i < totalTrees; ++i) {
            if (i % 10 >= 3) continue;           // only the escape slots exist
            while (!ready[i].load(std::memory_order_acquire)) std::this_thread::yield();
            const ObjHandle p = pub[i];
            if (!mEsc->rootAdd(p)) escFail.fetch_add(1, std::memory_order_relaxed);
            if (!mEsc->rootRemove(p)) escFail.fetch_add(1, std::memory_order_relaxed);
            ack[i].store(true, std::memory_order_release);
        }
    });

    owner.join();
    escaper.join();
    mOwner->collectLocal();                      // reclaim the owner's local tail
    rt.awaitQuiesce();                           // worker reclaims the escapes
    const double s = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    Result r;
    r.seconds = s;
    r.objects = objs.load();
    r.liveAtEnd = rt.liveCells();
    r.errors = static_cast<unsigned long long>(escFail.load());
    r.errors += mOwner->localEngine()->totalErrors();
    r.errors += mEsc->localEngine()->totalErrors();
    r.errors += rt.engine().totalErrors();
    r.promotions = rt.promotionsDone();
    r.reclaimed = rt.engine().cellsReclaimedZero() + rt.engine().cellsReclaimedCycle();
    return r;
}

struct Band {
    double minSec = 0;   // fastest rep (-> max rate)
    double maxSec = 0;   // slowest rep (-> min rate)
};

// Run a measurement `reps` times and return the MEDIAN-timed result, recording
// the fastest/slowest reps so the run-to-run spread is printed, not hidden.
template <class F>
Result medRun(int reps, Band& band, F&& fn) {
    std::vector<double> secs;
    secs.reserve(reps > 0 ? static_cast<size_t>(reps) : 1u);
    Result best;
    for (int i = 0; i < reps; ++i) {
        best = fn();
        secs.push_back(best.seconds);
    }
    std::sort(secs.begin(), secs.end());
    band.minSec = secs.front();
    band.maxSec = secs.back();
    best.seconds = secs[reps / 2];          // median
    return best;
}

void printMed(const char* label, int threads, const Result& med, const Band& band,
              bool showDiag) {
    const double rate = static_cast<double>(med.objects) / med.seconds;
    std::printf("  %-14s T=%-2d  %9.0f obj/s   %7.2f ms   [%9.0f .. %9.0f] obj/s "
                "live=%5u errs=%llu\n",
                label, threads, rate, med.seconds * 1e3,
                static_cast<double>(med.objects) / band.maxSec,
                static_cast<double>(med.objects) / band.minSec,
                med.liveAtEnd, (unsigned long long)med.errors);
    if (showDiag)
        std::printf("           diag: msgs=%llu spins=%llu rounds=%llu peakQ=%llu "
                    "reclaimed=%llu promotions=%llu\n",
                    med.msgs, med.spins, med.rounds, med.peakOcc,
                    med.reclaimed, med.promotions);
}

} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 150000;   // trees per thread
    const int reps = argc > 2 ? std::atoi(argv[2]) : 7;         // reps per cell (median)
    const int escapeTrees = argc > 3 ? std::atoi(argv[3]) : 6000;
    const int threadSet[] = {1, 4, 8};
    const int escapeThreads = 2;   // one owner + one escaper

    std::printf("bench_adaptive: %d trees/thread x %d reps (median), pool=%u cells\n",
                iters, reps, kPoolCap);
    std::printf("AARC_ENABLE_STATS=%d, Release; no LTO/PGO on this toolchain.\n"
                "obj/s = median; [.. ..] = min..max band across the reps.\n\n",
                int(AARC_ENABLE_STATS));

    std::printf("-- acyclic, pure thread-local (no escape) --\n");
    for (int threads : threadSet) {
        std::printf("  threads=%d\n", threads);
        Band bA, bB, bC;
        Result a = medRun(reps, bA, [&] { return acyclic_arc(threads, iters); });
        printMed("ARC", threads, a, bA, false);
        Result b = medRun(reps, bB, [&] { return acyclic_async(threads, iters); });
        printMed("AARC-async", threads, b, bB, true);
        Result c = medRun(reps, bC, [&] { return acyclic_adaptive_local(threads, iters); });
        printMed("Adapt-LOCAL", threads, c, bC, false);
    }

    std::printf("\n-- 30%% cross-thread escape (correctness + throughput) --\n");
    {
        Band b;
        const Result r = medRun(reps, b, [&] { return escape_30(escapeTrees); });
        printMed("Adapt-escape30", escapeThreads, r, b, true);
        std::printf("      expect: live=0, errs=0, promotions=%d\n",
                    escapeTrees * 3 / 10);
    }

    std::printf("\nnote: promotion closure now walks the persistent intrusive reverse\n"
                "      adjacency (component-time), not an O(pool) scan.  The escape row\n"
                "      still includes the promotion handshake; the pure thread-local row\n"
                "      is the adaptive thesis measurement.  A/B vs the legacy O(pool)\n"
                "      closure (AARC_LEGACY_POOL_CLOSURE) measured ~1.67x on this box.\n");
    return 0;
}
