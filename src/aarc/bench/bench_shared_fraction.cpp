// Adaptive AARC ablation over the SHARED fraction.
//
// Thesis: only objects that actually escape to a shared regime pay the async
// tax.  A thread-local (shared=0) workload must track inline ARC; as the shared
// fraction rises the regime tax should pull adaptive throughput toward uniform
// async; at shared=100% adaptive == uniform async.
//
// Design: each producer allocates an acyclic pair (p->c).  With probability f
// the pair is PRE-CLASSIFIED allocShared (worker-owned); otherwise allocLocal
// (inline).  f is chosen at alloc time deterministically.  This isolates the
// execution-regime cost; promotion latency itself is bench_promotion_scaling.
//
// Throughput is measured on a wall-clock budget so every cell takes about the
// same time regardless of f (pure-local is fast, fully-shared is slow).
// Columns: ARC (flat inline baseline), uniform async (flat), adaptive(f).
//
// Usage: bench_shared_fraction [budgetMs] [reps] [threads]

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
    double objPerSec = 0;
    double seconds = 0;
    long long objects = 0;
    unsigned long long errors = 0;
    unsigned long long promotions = 0;
    unsigned live = 0;
};

// Deterministic per-iteration shared decision: f is an integer percent.
inline bool isShared(long long i, int f) { return (i % 100) < f; }

template <class Body>
void runBudget(int threads, int budgetMs, Body&& body) {
    std::atomic<bool> stop{false};
    const double deadline = nowSec() + budgetMs / 1000.0;
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] { body(t, stop, deadline); });
    for (auto& t : ts) t.join();
}

// ARC: inline atomic RC, f-independent (acyclic body).
Row row_arc(int threads, int budgetMs) {
    BasicArc arc(1u << 20);
    std::atomic<long long> objs{0};
    runBudget(threads, budgetMs, [&](int, std::atomic<bool>& stop, double deadline) {
        while (nowSec() < deadline && !stop.load(std::memory_order_relaxed)) {
            ObjHandle p = arc.alloc();
            if (p == kNullHandle) { stop.store(true); break; }
            ObjHandle c = arc.alloc();
            if (c == kNullHandle) { arc.release(p); continue; }
            arc.addEdge(p, c);
            arc.release(c);
            arc.release(p);
            objs.fetch_add(2, std::memory_order_relaxed);
        }
    });
    Row r;
    r.objects = objs.load();
    r.objPerSec = r.objects / budgetMs * 1000.0;
    r.errors = arc.errorCount();
    r.live = arc.liveCells();
    return r;
}

// Uniform async: everything goes to the worker.
Row row_async(int threads, int budgetMs) {
    Runtime rt(1u << 20, Runtime::Mode::Async);
    std::vector<std::unique_ptr<Mutator>> ms(threads);
    std::atomic<long long> objs{0};
    runBudget(threads, budgetMs, [&](int me, std::atomic<bool>& stop,
                                     double deadline) {
        ms[me] = rt.makeMutator();
        while (nowSec() < deadline && !stop.load(std::memory_order_relaxed)) {
            ObjHandle p = ms[me]->alloc();
            if (p == kNullHandle) { stop.store(true); break; }
            ObjHandle c = ms[me]->alloc();
            if (c == kNullHandle) { ms[me]->rootRemove(p); continue; }
            ms[me]->edgeAdd(p, c);
            ms[me]->rootRemove(c);
            ms[me]->rootRemove(p);
            objs.fetch_add(2, std::memory_order_relaxed);
        }
    });
    rt.awaitQuiesce();
    Row r;
    r.objects = objs.load();
    r.objPerSec = r.objects / budgetMs * 1000.0;
    r.live = rt.liveCells();
    for (auto& m : ms)
        if (m) r.errors += m->localEngine() ? m->localEngine()->totalErrors() : 0;
    r.errors += rt.engine().totalErrors();
    return r;
}

// Adaptive: fraction f of pairs pre-classified SHARED, rest LOCAL.
Row row_adaptive(int threads, int budgetMs, int f) {
    Runtime rt(1u << 20, Runtime::Mode::Adaptive);
    std::vector<std::unique_ptr<Mutator>> ms(threads);
    std::atomic<long long> objs{0};
    runBudget(threads, budgetMs, [&](int me, std::atomic<bool>& stop,
                                     double deadline) {
        ms[me] = rt.makeMutator();
        long long i = 0;
        while (nowSec() < deadline && !stop.load(std::memory_order_relaxed)) {
            const bool shared = isShared(i, f);
            ObjHandle p = shared ? ms[me]->allocShared() : ms[me]->allocLocal();
            if (p == kNullHandle) { stop.store(true); break; }
            ObjHandle c = shared ? ms[me]->allocShared() : ms[me]->allocLocal();
            if (c == kNullHandle) { ms[me]->rootRemove(p); continue; }
            ms[me]->edgeAdd(p, c);
            ms[me]->rootRemove(c);
            ms[me]->rootRemove(p);
            objs.fetch_add(2, std::memory_order_relaxed);
            if (!shared && (i & 0xFF) == 0) ms[me]->collectLocal();
            ++i;
        }
    });
    for (auto& m : ms) m->collectLocal();
    rt.awaitQuiesce();
    Row r;
    r.objects = objs.load();
    r.objPerSec = r.objects / budgetMs * 1000.0;
    r.live = rt.liveCells();
    r.promotions = rt.promotionsDone();
    for (auto& m : ms) r.errors += m->localEngine()->totalErrors();
    r.errors += rt.engine().totalErrors();
    return r;
}

} // namespace

int main(int argc, char** argv) {
    const int budgetMs = argc > 1 ? std::atoi(argv[1]) : 800;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 5;
    const int threads = argc > 3 ? std::atoi(argv[3]) : 2;
    const int fractions[] = {0, 1, 5, 10, 30, 50, 100};

    std::printf("bench_shared_fraction: budget=%dms x%d reps, %d producer threads\n",
                budgetMs, reps, threads);
    std::printf("pre-classified regime per object pair (promotion cost is the "
                "other bench)\n\n");

    // Collect medians per cell over `reps` runs (report min..max implicitly via
    // the median; band printed separately to keep the table readable).
    Row arc = row_arc(threads, budgetMs);   // flat reference
    Row async = row_async(threads, budgetMs);

    std::printf("  %6s | %10s | %10s | %10s\n", "shared%", "ARC", "async",
                "adaptive");
    std::printf("  %6s | %10.0f | %10.0f | %10s\n", "flat", arc.objPerSec,
                async.objPerSec, "-");

    for (int f : fractions) {
        std::vector<double> secs;
        Row best;
        for (int r = 0; r < reps; ++r) {
            best = row_adaptive(threads, budgetMs, f);
            secs.push_back(best.objPerSec);
        }
        std::sort(secs.begin(), secs.end());
        const double med = secs[reps / 2];
        std::printf("  %6d | %10s | %10s | %10.0f   [%10.0f..%10.0f] live=%u errs=%llu\n",
                    f, "-", "-", med, secs.front(), secs.back(), best.live,
                    best.errors);
    }
    return 0;
}
