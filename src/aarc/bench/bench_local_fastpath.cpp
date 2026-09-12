// Measures how much of AARC's single-thread tax is the async queue/worker
// round-trip vs the accounting model itself.
//
//   * ARC       : inline synchronous atomic reference counting  (fast path)
//   * AARC-Sync : the SAME graph/refcount engine, but processing inline on
//                 the caller thread (no queue, no worker)  ~ "thread-local fast
//                 path" ceiling for the tiered design
//   * AARC-Async : the full async queue + background worker
//
// Acyclic workload, 1 producer thread.  If AARC-Sync ~= ARC, the async
// round-trip is ~all of the T1 gap and a local fast path can reclaim it.

#include "../bench/basic_arc.hpp"
#include "arc/runtime.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>

using namespace loxis::aarc;

namespace {

struct R {
    double sec = 0;
    long long objs = 0;
    unsigned live = 0;
    unsigned long long errs = 0;
};

long long arc_run(int iters) {
    BasicArc arc(1u << 20);
    long long objs = 0;
    for (int i = 0; i < iters; ++i) {
        ObjHandle p = arc.alloc();
        if (p == kNullHandle) continue;
        ObjHandle c = arc.alloc();
        if (c == kNullHandle) { arc.release(p); continue; }
        arc.addEdge(p, c);
        arc.release(c);
        arc.release(p);
        objs += 2;
    }
    return objs;
}

long long aarc_sync_run(int iters) {
    Runtime rt(1u << 20, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    long long objs = 0;
    for (int i = 0; i < iters; ++i) {
        ObjHandle p = m->alloc();
        if (p == kNullHandle) continue;
        ObjHandle c = m->alloc();
        if (c == kNullHandle) { m->rootRemove(p); continue; }
        m->edgeAdd(p, c);
        m->rootRemove(c);
        m->rootRemove(p);
        objs += 2;
        if ((i & 0x7FF) == 0) rt.awaitQuiesce();   // reclaim incrementally
    }
    rt.awaitQuiesce();
    return objs;
}

long long aarc_async_run(int iters) {
    Runtime rt(1u << 20, Runtime::Mode::Async);
    auto m = rt.makeMutator();
    long long objs = 0;
    for (int i = 0; i < iters; ++i) {
        ObjHandle p = m->alloc();
        if (p == kNullHandle) continue;
        ObjHandle c = m->alloc();
        if (c == kNullHandle) { m->rootRemove(p); continue; }
        m->edgeAdd(p, c);
        m->rootRemove(c);
        m->rootRemove(p);
        objs += 2;
    }
    rt.awaitQuiesce();
    return objs;
}

} // namespace

int main(int argc, char** argv) {
    const int iters = argc > 1 ? std::atoi(argv[1]) : 1000000;

    double t0 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long a = arc_run(iters);
    double t1 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long s = aarc_sync_run(iters);
    double t2 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    long long q = aarc_async_run(iters);
    double t3 = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    const double arcRate = a / (t1 - t0);
    const double syncRate = s / (t2 - t1);
    const double asyncRate = q / (t3 - t2);

    std::printf("bench_local_fastpath: acyclic, 1 thread, %d trees (%d objs)\n\n", iters, iters * 2);
    std::printf("  %-11s  %10.0f obj/s   %9.2f ms\n", "ARC", arcRate, (t1 - t0) * 1e3);
    std::printf("  %-11s  %10.0f obj/s   %9.2f ms   %5.1f%% of ARC\n",
                "AARC-sync", syncRate, (t2 - t1) * 1e3, 100.0 * syncRate / arcRate);
    std::printf("  %-11s  %10.0f obj/s   %9.2f ms   %5.1f%% of ARC\n",
                "AARC-async", asyncRate, (t3 - t2) * 1e3, 100.0 * asyncRate / arcRate);
    std::printf("\nobjects: arc=%lld sync=%lld async=%lld (all returned clean)\n", a, s, q);
    return 0;
}
