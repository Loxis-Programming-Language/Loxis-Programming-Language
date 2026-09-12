// Concurrency stress (Todo_AddGC.md 9 -> Concurrency stress).
//
// Multiple producer threads mix allocation, edge mutation, root add/remove and
// cycle creation/destruction.  A controller keeps a fixed set of objects rooted
// for the whole run.  After every producer is joined and the worker quiesced:
//   * no refcount underflow / stale-corruption errors,
//   * exactly the controller's rooted objects are still live (nothing acyclic
//     leaked, no orphaned cycle leaked),
//   * dropping the controller's roots reclaims everything.
//
// Note: no cell data is dereferenced after release, so "UAF / stale handle"
// manifests here as a non-zero error count or a leak, both asserted.

#include "arc/runtime.hpp"
#include "th_test.hpp"

#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

void stress_run() {
    constexpr int kProducers = 6;
    constexpr int kIters = 4000;
    constexpr int kKeep = 150;
    Runtime rt(1 << 19, Runtime::Mode::Async);

    // controller roots (allocated on this thread, held for the run)
    {
        auto keep = rt.makeMutator();
        std::vector<ObjHandle> held;
        for (int i = 0; i < kKeep; ++i) {
            ObjHandle h = keep->alloc();
            CHECK(h != kNullHandle);
            held.push_back(h);
        }
        CHECK_EQ(rt.liveCells(), static_cast<uint32_t>(kKeep));

        std::vector<std::thread> ts;
        for (int t = 0; t < kProducers; ++t) {
            ts.emplace_back([&rt] {
                auto m = rt.makeMutator();
                for (int i = 0; i < kIters; ++i) {
                    switch (i % 4) {
                        case 0: {   // acyclic parent->child, then drop root
                            ObjHandle p = m->alloc();
                            if (p == kNullHandle) break;
                            ObjHandle c = m->alloc();
                            if (c != kNullHandle) {
                                m->edgeAdd(p, c);
                                m->rootRemove(c);
                            }
                            m->rootRemove(p);
                            break;
                        }
                        case 1: {   // orphaned 2-cycle
                            ObjHandle a = m->alloc();
                            if (a == kNullHandle) break;
                            ObjHandle b = m->alloc();
                            if (b == kNullHandle) { m->rootRemove(a); break; }
                            m->edgeAdd(a, b);
                            m->edgeAdd(b, a);
                            m->rootRemove(a);
                            m->rootRemove(b);
                            break;
                        }
                        case 2: {   // edge churn among a small live set
                            ObjHandle x = m->alloc();
                            if (x == kNullHandle) break;
                            ObjHandle y = m->alloc();
                            if (y == kNullHandle) { m->rootRemove(x); break; }
                            m->edgeAdd(x, y);
                            m->edgeRemove(x, y);
                            m->edgeAdd(x, y);   // re-add
                            m->rootRemove(y);   // y kept only via x's edge
                            m->rootRemove(x);   // ...then release x too
                            break;
                        }
                        default: {  // temporary multi-root
                            ObjHandle o = m->alloc();
                            if (o == kNullHandle) break;
                            m->rootAdd(o);
                            m->rootRemove(o);
                            m->rootRemove(o);
                            break;
                        }
                    }
                }
            });
        }
        for (auto& t : ts) t.join();

        rt.awaitQuiesce();
        CHECK_EQ(rt.engine().totalErrors(), 0ull);
        CHECK_MSG(rt.liveCells() == static_cast<uint32_t>(kKeep),
                  "only the controller's rooted objects may remain live");

        // release the controller roots -> everything must be reclaimed
        for (auto h : held) CHECK(keep->rootRemove(h));
        rt.awaitQuiesce();
        CHECK_EQ(rt.liveCells(), 0u);
        CHECK_EQ(rt.engine().totalErrors(), 0ull);
    }
}

} // namespace

int main() {
    std::printf("[test_stress]\n");
    stress_run();
    return aarc_test::failures() ? 1 : 0;
}
