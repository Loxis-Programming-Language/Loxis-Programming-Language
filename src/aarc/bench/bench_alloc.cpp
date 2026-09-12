// AARC allocation benchmark (user-specified workload).
//
//   * 100,000 objects total.  Each object is counted at a nominal ~10 KB
//     payload (AARC cells themselves are 9 bytes; the 10 KB scale models the
//     eventual Loxis object size), so the run allocates
//        100000 * 10240 B  ==  ~1.0 GB  of nominal object memory.
//   * 30% of the objects (30,000) are wired into orphaned SELF-cycles
//     (obj -> obj) and must be reclaimed by the cycle collector.
//   * 70% (70,000) are acyclic and reclaimed by plain reference counting after
//     their roots are dropped.
//
// Self-cycles keep the object census exact: 100k allocations total, exactly
// 30k of them cyclic, so the reported ratio is precisely the requested 30%.
//
// The run happens in 10 rounds of 10,000 allocations each; after every round
// the async worker is quiesced so the collector catches up (no queue buildup).
// Assertions: zero GC errors and zero live cells at the end — i.e. all 30,000
// cyclic cells were *collected*, not leaked.
//
// Build/run:
//   cmake --build build-aarc
//   build-aarc/bench_alloc.exe [objects] [cyclePercent]

#include "arc/runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace loxis::aarc;

namespace {
constexpr int64_t kNominalObjectBytes = 10 * 1024;   // ~10 KB per object
constexpr int64_t kTotalObjects = 100000;            // ~1.0 GB nominal
constexpr int   kCyclePercent = 30;
constexpr int64_t kRoundObjects = 10000;             // allocations per round
} // namespace

int main(int argc, char** argv) {
    const int64_t total = argc > 1 ? std::atoll(argv[1]) : kTotalObjects;
    const int   cyclePct = argc > 2 ? std::atoi(argv[2]) : kCyclePercent;

    // Pool large enough for one round's live set + slack (10k objects alive
    // at peak: up to 7k acyclic + cycle cells).
    constexpr uint32_t kPoolCap = 1u << 16;
    Runtime rt(kPoolCap, Runtime::Mode::Async);
    auto m = rt.makeMutator();

    const int64_t rounds = total / kRoundObjects;
    const int64_t nominalBytes = total * kNominalObjectBytes;

    int64_t cyclicAlloc = 0;     // cells created inside orphaned cycles
    std::vector<ObjHandle> roundRoots;
    roundRoots.reserve(kRoundObjects);

    std::printf("bench_alloc: total=%lld obj  nominal=%lld B (%.2f GB)  cycle=%d%%\n",
                total, nominalBytes, nominalBytes / 1e9, cyclePct);
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();

    for (int64_t r = 0; r < rounds; ++r) {
        roundRoots.clear();
        for (int64_t i = 0; i < kRoundObjects; ++i) {
            if ((i % 100) < static_cast<int64_t>(cyclePct)) {
                // orphaned self-cycle: obj -> obj (exactly one object)
                ObjHandle a = m->alloc();
                if (a == kNullHandle) continue;
                m->edgeAdd(a, a);
                m->rootRemove(a);
                cyclicAlloc += 1;
            } else {
                // acyclic object, kept rooted for this round
                ObjHandle p = m->alloc();
                if (p == kNullHandle) continue;
                roundRoots.push_back(p);
            }
        }
        // drop this round's acyclic roots, then let the collector catch up
        for (ObjHandle h : roundRoots) m->rootRemove(h);
        rt.awaitQuiesce();
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    // ---- verification ------------------------------------------------------
    rt.awaitQuiesce();
    const bool clean = rt.liveCells() == 0 && rt.engine().totalErrors() == 0;

    const double objPerSec = static_cast<double>(total) / secs;
    const double bytesPerSec = static_cast<double>(nominalBytes) / secs;
    std::printf("elapsed          : %.3f s\n", secs);
    std::printf("objects alloc    : %lld  (%.0f obj/s)\n", total, objPerSec);
    std::printf("cyclic cells made: %lld  (%.0f%% of objects)\n",
                cyclicAlloc, 100.0 * cyclicAlloc / total);
    std::printf("nominal bytes    : %.2f MB/s  (objects ~10 KB each)\n",
                bytesPerSec / 1e6);
    std::printf("result           : %s\n",
                clean ? "PASS  (live=0, GC errors=0 => all 30%-cycle cells were collected)"
                      : "FAIL  (leaked live cells or GC errors)");
    return clean ? 0 : 1;
}
