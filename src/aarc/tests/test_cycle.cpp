// Phase 2 tests: cyclic garbage detection & reclamation (Todo_AddGC.md 9 ->
// Cycle detection).
//
// The mandated topologies:
//   * A -> B -> A                    reclaimed
//   * A -> B -> C -> A               reclaimed
//   * root -> A -> B -> A            NOT reclaimed while the root lives
//   * A -> B -> A , C -> B           NOT reclaimed while C lives
//   * A <-> B -> C <-> D             SCC membership is correct; all reclaimed
//                                     once no root reaches them

#include "arc/runtime.hpp"
#include "th_test.hpp"

using namespace loxis::aarc;

namespace {

void two_node_cycle() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->rootRemove(a));
    CHECK(m->rootRemove(b));

    CHECK_EQ(rt.engine().refcountOf(a), 1);   // only b->a
    CHECK_EQ(rt.engine().refcountOf(b), 1);   // only a->b
    rt.awaitQuiesce();                        // cycle scan
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

void three_node_cycle() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    ObjHandle c = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, c));
    CHECK(m->edgeAdd(c, a));
    m->rootRemove(a);
    m->rootRemove(b);
    m->rootRemove(c);
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK(!rt.pool().valid(c));
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// root -> A -> B -> A: must NOT be reclaimed while the root lives.
void rooted_cycle_not_reclaimed() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->rootRemove(b));                  // keep a's alloc root = "root"
    rt.awaitQuiesce();
    CHECK(rt.pool().valid(a));
    CHECK(rt.pool().valid(b));
    CHECK_EQ(rt.engine().refcountOf(a), 2);   // root + b->a

    CHECK(m->rootRemove(a));                  // now orphaned
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// A -> B -> A with a live external C -> B: must not be reclaimed.
void external_edge_keeps_cycle() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    ObjHandle c = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->edgeAdd(c, b));                  // external reference into cycle
    m->rootRemove(a);
    m->rootRemove(b);
    rt.awaitQuiesce();
    CHECK(rt.pool().valid(a));
    CHECK(rt.pool().valid(b));
    CHECK_EQ(rt.engine().refcountOf(b), 2);   // a->b + c->b

    // C's reclamation orphans the cycle (no explicit remove message for the
    // edge — it dies with C).  The engine must notice and reclaim it.
    CHECK(m->rootRemove(c));
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK(!rt.pool().valid(c));
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// A <-> B -> C <-> D : two SCCs chained.  Once no root reaches them, both
// components must be reclaimed (the first pass reclaims A<->B, which orphans
// C<->D via the B->C edge; the engine must keep scanning until done).
void chained_sccs() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    ObjHandle c = m->alloc();
    ObjHandle d = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->edgeAdd(b, c));                  // bridge A<->B --> C<->D
    CHECK(m->edgeAdd(c, d));
    CHECK(m->edgeAdd(d, c));
    m->rootRemove(a);
    m->rootRemove(b);
    m->rootRemove(c);
    m->rootRemove(d);

    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK(!rt.pool().valid(c));
    CHECK(!rt.pool().valid(d));
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// A cycle that also points outward at a live object: only the cycle members
// die; the outward target survives.
void cycle_with_live_out_target() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    ObjHandle target = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->edgeAdd(b, target));             // cycle -> live target
    m->rootRemove(a);
    m->rootRemove(b);
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK(rt.pool().valid(target));           // its own root keeps it alive
    CHECK_EQ(rt.engine().refcountOf(target), 1);
    m->rootRemove(target);
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(target));
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// The zero sweep and the cycle pass must be driven to a fixed point.  Here the
// cycle C is kept alive by B, and B is kept alive only by an edge from cycle A.
// Reclaiming A zeroes B; the *final* sweepZero frees B and thereby removes C's
// last external edge -- which marks C suspect AFTER collectCycles has already
// run.  Without the fixed-point loop that suspect waits for the next drain
// point, i.e. it leaks for good once the program quiesces.
void cycle_orphaned_by_zero_sweep_is_reclaimed() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a1 = m->alloc();
    ObjHandle a2 = m->alloc();
    ObjHandle b = m->alloc();
    ObjHandle c1 = m->alloc();
    ObjHandle c2 = m->alloc();
    CHECK(m->edgeAdd(a1, a2));
    CHECK(m->edgeAdd(a2, a1));                // cycle A
    CHECK(m->edgeAdd(a1, b));                 // A -> B (B's only reference)
    CHECK(m->edgeAdd(b, c1));                 // B -> C (C's only external ref)
    CHECK(m->edgeAdd(c1, c2));
    CHECK(m->edgeAdd(c2, c1));                // cycle C
    for (ObjHandle h : {a1, a2, b, c1, c2}) CHECK(m->rootRemove(h));

    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(a1));
    CHECK(!rt.pool().valid(a2));
    CHECK(!rt.pool().valid(b));
    CHECK(!rt.pool().valid(c1));
    CHECK(!rt.pool().valid(c2));
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Cyclic garbage under async producer threads must also be collected.
void async_cycle_stress() {
    constexpr int kThreads = 4;
    constexpr int kCycles = 500;
    Runtime rt(1 << 17, Runtime::Mode::Async);

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&rt] {
            auto m = rt.makeMutator();
            for (int i = 0; i < kCycles; ++i) {
                ObjHandle a = m->alloc();
                ObjHandle b = m->alloc();
                if (a == kNullHandle || b == kNullHandle) continue;
                m->edgeAdd(a, b);
                m->edgeAdd(b, a);
                m->rootRemove(a);
                m->rootRemove(b);
            }
        });
    }
    for (auto& t : ts) t.join();
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

} // namespace

int main() {
    std::printf("[test_cycle]\n");
    two_node_cycle();
    three_node_cycle();
    rooted_cycle_not_reclaimed();
    external_edge_keeps_cycle();
    chained_sccs();
    cycle_with_live_out_target();
    cycle_orphaned_by_zero_sweep_is_reclaimed();
    async_cycle_stress();
    return aarc_test::failures() ? 1 : 0;
}
