// M10a/M10b: SHARED async transport ablation.
//
// The adaptive design already made the LOCAL path cheap; the remaining wall is
// the SHARED path's per-message transport tax: publication bookkeeping
// (pubSeq window), a global work-epoch bump, a worker wake, and the worker's
// per-round overhead.  This bench isolates exactly that tax on one workload
// (a uniform-async acyclic pair, 5 messages / 2 objects) while sweeping the
// producer batch size in ONE binary, interleaved rep-by-rep so turbo/thermal
// drift hits every cell the same way.
//
//   throughput: obj/s, msgs/s, batches/s, avg msgs/batch, wakeups, spins,
//               rounds, peak queue, live, errs
//   latency   : quiesce round-trip (flush -> worker round -> ticket complete),
//               median and p99 (the synchronous "GC must finish now" case)
//
// Usage: bench_async_transport [iters] [reps] [threads] [latSamples]

#include "../bench/basic_arc.hpp"
#include "arc/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
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

struct Row {
    double seconds = 0;
    long long objects = 0;
    uint64_t msgs = 0;          // process() calls the worker actually made
    uint64_t published = 0;     // messages pushed onto the rings
    uint64_t foldIn = 0;        // staged root/edge ops
    uint64_t foldOut = 0;       // emitted root/edge ops
    uint64_t batches = 0;
    uint64_t wakeups = 0;
    uint64_t spins = 0;
    uint64_t rounds = 0;
    uint64_t peakQ = 0;
    uint32_t live = 0;
    uint64_t errs = 0;
};

// One uniform-async throughput cell: every mutation goes through the worker.
Row runAsync(int threads, int iters, uint32_t batchSize, bool fold,
             bool spanPath = true, bool localCache = true) {
    Runtime rt(1u << 20, Runtime::Mode::Async, true, batchSize, fold, spanPath);
    rt.setLocalCache(localCache);
    std::atomic<long long> objs{0};
    const double t0 = nowSec();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&] {
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
    }
    for (auto& t : ts) t.join();
    rt.awaitQuiesce();
    const double s = nowSec() - t0;

    Row r;
    r.seconds = s;
    r.objects = objs.load();
    r.msgs = rt.engine().msgsProcessed();
    r.published = rt.msgsPublished();
    r.foldIn = rt.foldStats().inTotal();
    r.foldOut = rt.foldStats().outTotal();
    r.batches = rt.batchesPublished();
    r.wakeups = rt.wakeups();
    r.spins = rt.spinCount();
    r.rounds = rt.roundsDone();
    r.peakQ = rt.peakQueueOccupancy();
    r.live = rt.liveCells();
    r.errs = rt.engine().totalErrors();
    return r;
}

// ARC reference (flat inline baseline, same workload).
Row runArc(int threads, int iters) {
    BasicArc arc(1u << 20);
    std::atomic<long long> objs{0};
    const double t0 = nowSec();
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&] {
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
    }
    for (auto& t : ts) t.join();
    Row r;
    r.seconds = nowSec() - t0;
    r.objects = objs.load();
    r.live = arc.liveCells();
    r.errs = arc.errorCount();
    return r;
}

// Quiesce round-trip latency: produce one small batch, publish it, wait until
// the worker has applied it at a stable cut.  This is the synchronous case
// (no producer/worker overlap), so it measures wake + round + sweep.
struct Lat {
    double medUs = 0;
    double p99Us = 0;
};

Lat runLatency(uint32_t batchSize, bool fold, int samples) {
    Runtime rt(1u << 16, Runtime::Mode::Async, true, batchSize, fold);
    auto m = rt.makeMutator();
    std::vector<double> v;
    v.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        const double t0 = nowSec();
        ObjHandle p = m->alloc();
        ObjHandle c = m->alloc();
        if (p == kNullHandle || c == kNullHandle) break;
        m->edgeAdd(p, c);
        m->rootRemove(c);
        m->rootRemove(p);
        m->flush();
        rt.awaitQuiesce();
        v.push_back((nowSec() - t0) * 1e6);
    }
    std::sort(v.begin(), v.end());
    Lat l;
    if (!v.empty()) {
        l.medUs = v[v.size() / 2];
        l.p99Us = v[(v.size() * 99) / 100];
    }
    return l;
}

void printThroughput(const char* label, const Row& r) {
    const double rate = r.objects / r.seconds;
    const double mrate = r.msgs / r.seconds;
    const double brate = r.batches / r.seconds;
    const double avg = r.batches ? double(r.published) / double(r.batches) : 0.0;
    std::printf("  %-14s %9.0f obj/s  %10.0f proc/s  %10.0f batch/s  avg=%6.2f  "
                "wake=%-8llu spin=%-8llu rounds=%-7llu peakQ=%-6llu "
                "live=%u errs=%llu\n",
                label, rate, mrate, brate, avg,
                (unsigned long long)r.wakeups, (unsigned long long)r.spins,
                (unsigned long long)r.rounds, (unsigned long long)r.peakQ,
                r.live, (unsigned long long)r.errs);
}

} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 120000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 7;
    const int threads = argc > 3 ? std::atoi(argv[3]) : 1;
    const int latSamples = argc > 4 ? std::atoi(argv[4]) : 4000;
    const uint32_t sizes[] = {1, 8, 16, 32, 64, 128};

    std::printf("bench_async_transport: %d pairs/thread x %d reps (median), T=%d\n",
                iters, reps, threads);
    std::printf("workload: alloc p, alloc c, edgeAdd p->c, rootRemove c, rootRemove p"
                "  (5 msgs / 2 objs)\n\n");

    std::printf("-- ARC reference --\n");
    {
        std::vector<double> secs;
        Row last;
        for (int r = 0; r < reps; ++r) { last = runArc(threads, iters); secs.push_back(last.seconds); }
        std::sort(secs.begin(), secs.end());
        last.seconds = secs[reps / 2];
        printThroughput("ARC", last);
    }

    std::printf("\n-- uniform async, batch sweep (interleaved reps) --\n");
    std::vector<std::vector<double>> objSecs(sizeof(sizes) / sizeof(sizes[0]));
    std::vector<Row> best(sizeof(sizes) / sizeof(sizes[0]));
    for (int r = 0; r < reps; ++r) {
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            Row row = runAsync(threads, iters, sizes[i], false);
            objSecs[i].push_back(row.objects / row.seconds);
            best[i] = row;
        }
    }
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        std::sort(objSecs[i].begin(), objSecs[i].end());
        best[i].seconds = best[i].objects / objSecs[i][reps / 2];   // median rate
        char label[32];
        std::snprintf(label, sizeof(label), "batch=%u", sizes[i]);
        printThroughput(label, best[i]);
        std::printf("               band [%.0f .. %.0f] obj/s\n",
                    objSecs[i].front(), objSecs[i].back());
    }

    std::printf("\n-- batching vs batching+folding (interleaved reps) --\n");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (sizes[i] < 8) continue;
        std::vector<double> plain, folded;
        Row bp, bf;
        // Alternate the order: a fixed order lets within-rep drift look like a
        // difference between the variants.
        for (int r = 0; r < reps; ++r) {
            if (r & 1) {
                bf = runAsync(threads, iters, sizes[i], true);
                folded.push_back(bf.objects / bf.seconds);
                bp = runAsync(threads, iters, sizes[i], false);
                plain.push_back(bp.objects / bp.seconds);
            } else {
                bp = runAsync(threads, iters, sizes[i], false);
                plain.push_back(bp.objects / bp.seconds);
                bf = runAsync(threads, iters, sizes[i], true);
                folded.push_back(bf.objects / bf.seconds);
            }
        }
        std::sort(plain.begin(), plain.end());
        std::sort(folded.begin(), folded.end());
        bp.seconds = bp.objects / plain[reps / 2];
        bf.seconds = bf.objects / folded[reps / 2];
        char label[32];
        std::snprintf(label, sizeof(label), "batch=%u", sizes[i]);
        printThroughput(label, bp);
        std::snprintf(label, sizeof(label), "batch=%u+fold", sizes[i]);
        printThroughput(label, bf);
        std::printf("                 staged=%llu emitted=%llu processed=%llu  "
                    "fold-ratio=%.2fx  speedup=%.2fx\n",
                    (unsigned long long)bf.foldIn, (unsigned long long)bf.foldOut,
                    (unsigned long long)bf.msgs,
                    bf.foldOut ? double(bf.foldIn) / double(bf.foldOut) : 0.0,
                    plain[reps / 2] ? folded[reps / 2] / plain[reps / 2] : 0.0);
    }

    std::printf("\n-- quiesce round-trip latency (%d samples, sync) --\n", latSamples);
    std::printf("  %-12s %10s %10s\n", "batch", "median us", "p99 us");
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const Lat l = runLatency(sizes[i], false, latSamples);
        std::printf("  %-12u %10.2f %10.2f\n", sizes[i], l.medUs, l.p99Us);
    }
    std::printf("  (folding does not change the round trip: same flush points)\n");
    return 0;
}
