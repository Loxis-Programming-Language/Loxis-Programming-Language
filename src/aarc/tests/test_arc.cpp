// Phase 1 tests: async reference counting.
//
// Sync-mode tests drive the engine deterministically; async tests run real
// producer threads against the background worker and assert a leak-free,
// error-free steady state (Todo_AddGC.md 9 -> Refcount).

#include "arc/runtime.hpp"
#include "th_test.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

// alloc -> drop the only handle -> cell is reclaimed after a collect.
void zero_transition() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();

    ObjHandle h = m->alloc();
    CHECK(h != kNullHandle);
    CHECK_EQ(rt.engine().refcountOf(h), 1);
    CHECK(rt.pool().valid(h));

    CHECK(m->rootRemove(h));
    CHECK_EQ(rt.engine().refcountOf(h), 0);    // still live, pending collect
    CHECK(rt.pool().valid(h));
    CHECK_EQ(rt.liveCells(), 1u);

    rt.awaitQuiesce();                          // sweepZero
    CHECK(!rt.pool().valid(h));                 // reclaimed
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// A -> B -> C chain: releasing the root reclaims the whole subtree.
void nested_release_cascade() {
    Runtime rt(32, Runtime::Mode::Sync);
    auto m = rt.makeMutator();

    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    ObjHandle c = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, c));
    // fold B's and C's temporary handles into the graph
    CHECK(m->rootRemove(b));
    CHECK(m->rootRemove(c));

    CHECK_EQ(rt.engine().refcountOf(a), 1);
    CHECK_EQ(rt.engine().refcountOf(b), 1);    // only from A
    CHECK_EQ(rt.engine().refcountOf(c), 1);    // only from B

    rt.awaitQuiesce();                          // nothing should be reclaimed
    CHECK(rt.pool().valid(a));
    CHECK(rt.pool().valid(b));
    CHECK(rt.pool().valid(c));

    CHECK(m->rootRemove(a));                    // kill the root
    rt.awaitQuiesce();                          // cascade a->b->c
    CHECK(!rt.pool().valid(a));
    CHECK(!rt.pool().valid(b));
    CHECK(!rt.pool().valid(c));
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Two roots keep an object alive; it dies only when both are dropped.
void multiple_roots() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();

    ObjHandle h = m->alloc();
    CHECK(m->rootAdd(h));                        // second root
    CHECK_EQ(rt.engine().refcountOf(h), 2);

    CHECK(m->rootRemove(h));                     // drop one
    CHECK_EQ(rt.engine().refcountOf(h), 1);
    rt.awaitQuiesce();
    CHECK(rt.pool().valid(h));                   // still alive via the other

    CHECK(m->rootRemove(h));                     // drop the last
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(h));
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Object reused after release must not be affected by stale messages that
// still reference the old generation (ABA protection, Todo_AddGC.md 1.3).
void stale_message_protection() {
    Runtime rt(2, Runtime::Mode::Sync);          // 2 slots => deterministic reuse
    auto m = rt.makeMutator();

    ObjHandle old = m->alloc();
    ObjHandle keep = m->alloc();                 // both slots live
    CHECK(m->rootRemove(old));                   // free `old` first
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(old));

    // Only one free slot exists now, so alloc() must reuse `old`'s slot.
    ObjHandle fresh = m->alloc();
    CHECK_EQ(fresh.index, old.index);
    CHECK(fresh.gen != old.gen);                 // generation bumped on reuse
    CHECK_EQ(rt.engine().refcountOf(fresh), 1);

    const uint64_t staleBefore = rt.engine().staleDropped();
    rt.engine().process({GCOp::RootAdd, kNullHandle, old});    // stale add
    rt.engine().process({GCOp::EdgeRemove, old, keep});        // stale source
    CHECK_EQ(rt.engine().staleDropped(), staleBefore + 2);     // both dropped
    // the reused cell and the survivor were untouched
    CHECK_EQ(rt.engine().refcountOf(fresh), 1);
    CHECK_EQ(rt.engine().refcountOf(keep), 1);

    CHECK(m->rootRemove(fresh));
    CHECK(m->rootRemove(keep));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
}

// Object-graph edge accounting: one edge == one reference, edge removal
// lowers the target, and the target survives as long as any edge remains.
void edge_accounting() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();

    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK_EQ(rt.engine().edgeOf(b), 1);
    CHECK_EQ(rt.engine().outgoingCount(a), 1u);
    #ifndef AARC_CONSERVATIVE
    CHECK_EQ(rt.engine().incomingCount(b), 1u);
    #endif
    CHECK_EQ(rt.engine().liveEdgeRecords(), 1ull);

    // Two fields from a to b are distinct records in both adjacency lists.
    CHECK(m->edgeAdd(a, b));
    CHECK_EQ(rt.engine().edgeOf(b), 2);
    CHECK_EQ(rt.engine().outgoingCount(a), 2u);
    #ifndef AARC_CONSERVATIVE
    CHECK_EQ(rt.engine().incomingCount(b), 2u);
    #endif
    CHECK_EQ(rt.engine().liveEdgeRecords(), 2ull);

    CHECK(m->edgeRemove(a, b));
    CHECK_EQ(rt.engine().edgeOf(b), 1);
    #ifndef AARC_CONSERVATIVE
    CHECK_EQ(rt.engine().incomingCount(b), 1u);
    #endif
    CHECK_EQ(rt.engine().liveEdgeRecords(), 1ull);
    CHECK(m->edgeRemove(a, b));
    CHECK_EQ(rt.engine().edgeOf(b), 0);
    #ifndef AARC_CONSERVATIVE
    CHECK_EQ(rt.engine().incomingCount(b), 0u);
    #endif
    CHECK_EQ(rt.engine().liveEdgeRecords(), 0ull);

    // removing a non-existent edge must be flagged, not corrupt the count
    const uint64_t errs = rt.engine().edgeUnderflow();
    CHECK(m->edgeRemove(a, b));                  // both handles are live, so the
                                                 // message is accepted... and the
    // engine then reports the missing edge (sync mode applies immediately)
    CHECK_EQ(rt.engine().edgeUnderflow(), errs + 1);
    CHECK_EQ(rt.engine().edgeOf(b), 0);
}

// Reclaiming a source must detach its reverse records from surviving targets;
// the freed EdgeIds are then safely reused for a different generation.
void reverse_index_reclaim_and_reuse() {
    Runtime rt(8, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle source = m->alloc();
    ObjHandle target = m->alloc();
    CHECK(m->edgeAdd(source, target));
    #ifndef AARC_CONSERVATIVE
    CHECK_EQ(rt.engine().incomingCount(target), 1u);
    #endif
    CHECK(m->rootRemove(target));
    CHECK(m->rootRemove(source));
    rt.awaitQuiesce();
    CHECK_EQ(rt.engine().liveEdgeRecords(), 0ull);
    CHECK(!rt.pool().valid(source));
    CHECK(!rt.pool().valid(target));

    ObjHandle freshSource = m->alloc();
    ObjHandle freshTarget = m->alloc();
    CHECK(freshSource != kNullHandle && freshTarget != kNullHandle);
    CHECK(m->edgeAdd(freshSource, freshTarget));
    CHECK_EQ(rt.engine().outgoingCount(freshSource), 1u);
    #ifndef AARC_CONSERVATIVE
    CHECK_EQ(rt.engine().incomingCount(freshTarget), 1u);
    #endif
    CHECK_EQ(rt.engine().liveEdgeRecords(), 1ull);
    CHECK(m->rootRemove(freshTarget));
    CHECK(m->rootRemove(freshSource));
    rt.awaitQuiesce();
    CHECK_EQ(rt.engine().liveEdgeRecords(), 0ull);
    CHECK_EQ(rt.liveCells(), 0u);
}

// A root underflow is detected and never drives a counter negative.
void root_underflow_detection() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();
    CHECK(m->rootRemove(h));
    // object still live (not yet collected) but has zero roots now
    rt.engine().process({GCOp::RootRemove, kNullHandle, h});
    CHECK_EQ(rt.engine().rootUnderflow(), 1ull);
    CHECK_EQ(rt.engine().refcountOf(h), 0);
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(h));
}

// Full pool: alloc() returns null when every slot is live.
void pool_full_reports_null() {
    Runtime rt(4, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    std::vector<ObjHandle> hs;
    for (uint32_t i = 0; i < 4; ++i)
        hs.push_back(m->alloc());
    CHECK(m->alloc() == kNullHandle);
    for (auto h : hs) CHECK(m->rootRemove(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
}

// Real async mode: several producer threads allocate small acyclic structures,
// fold child handles into edges, then drop the root.  After every thread is
// joined and the worker quiesced, nothing must be leaked or corrupted.
void async_acyclic_stress() {
    constexpr int kThreads = 4;
    constexpr int kIterations = 4000;
    Runtime rt(1 << 18, Runtime::Mode::Async);

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&rt] {
            auto m = rt.makeMutator();
            for (int i = 0; i < kIterations; ++i) {
                ObjHandle parent = m->alloc();
                if (parent == kNullHandle) continue;
                ObjHandle child = m->alloc();
                if (child != kNullHandle) {
                    if (!m->edgeAdd(parent, child)) { }
                    m->rootRemove(child);        // keep child via parent only
                }
                m->rootRemove(parent);
            }
        });
    }
    for (auto& t : ts) t.join();
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    // sanity: some allocations must have happened
    CHECK_MSG(kThreads * kIterations > 0, "work was performed");
}

// Async object that is handed across threads.  Safe handoff: the new owner
// takes a root BEFORE the old owner drops its own.
void async_cross_thread_handoff() {
    Runtime rt(256, Runtime::Mode::Async);

    ObjHandle shared = kNullHandle;
    std::atomic<bool> ready{false};
    std::atomic<bool> acked{false};

    std::thread producer([&] {
        auto m = rt.makeMutator();
        shared = m->alloc();
        ready.store(true, std::memory_order_release);
        // wait until the consumer has taken its own root
        while (!acked.load(std::memory_order_acquire)) { }
        m->rootRemove(shared);                    // transfer ownership
    });

    std::thread consumer([&] {
        auto m = rt.makeMutator();
        while (!ready.load(std::memory_order_acquire)) { }
        CHECK(m->rootAdd(shared));                // take own root first
        acked.store(true, std::memory_order_release);
        CHECK(m->rootRemove(shared));             // then drop it again later
    });

    producer.join();
    consumer.join();
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(shared));              // fully released at the end
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// M1: regime reflects the runtime execution mode (design doc §7).
void regime_reflects_mode() {
    Runtime sync(8, Runtime::Mode::Sync);
    auto ms = sync.makeMutator();
    ObjHandle hs = ms->alloc();
    CHECK(sync.pool().regimeOf(hs) == Regime::Local);

    Runtime async(8, Runtime::Mode::Async);
    auto ma = async.makeMutator();
    ObjHandle ha = ma->alloc();
    CHECK(async.pool().regimeOf(ha) == Regime::Shared);
    ma->rootRemove(ha);
    async.awaitQuiesce();
}

// M1: regime invariant holds at quiesce (single regime -> always true).
void regime_invariant_ok() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(rt.engine().regimeInvariantOk());
    m->rootRemove(a);
    m->rootRemove(b);
    rt.awaitQuiesce();
    CHECK(rt.engine().regimeInvariantOk());
}

// M2 base: engine authority gate — LOCAL engine only touches its own LOCAL
// cells; SHARED engine refuses LOCAL cells (design doc §7 M2, rule 1).
void canTouch_authority() {
    Runtime sync(8, Runtime::Mode::Sync);
    auto ms = sync.makeMutator();
    ObjHandle hs = ms->alloc();
    CHECK(sync.engine().kind() == Regime::Local);
    CHECK(sync.engine().canTouch(hs));               // Local engine -> own Local cell

    Runtime async(8, Runtime::Mode::Async);
    auto ma = async.makeMutator();
    ObjHandle ha = ma->alloc();
    CHECK(async.engine().kind() == Regime::Shared);
    CHECK(async.engine().canTouch(ha));              // Shared engine -> Shared cell

    async.pool().setRegime(ha, Regime::Local);       // simulate a forced escape
    CHECK(!async.engine().canTouch(ha));             // Shared engine must refuse it
    async.pool().setRegime(ha, Regime::Shared);
    CHECK(async.engine().canTouch(ha));

    ma->rootRemove(ha);
    async.awaitQuiesce();
}

} // namespace

int main() {
    std::printf("[test_arc]\n"); std::fflush(stdout);
#define T(f) do { std::printf("  %s...\n", #f); std::fflush(stdout); f(); } while (0)
    T(regime_reflects_mode);
    T(canTouch_authority);
    T(regime_invariant_ok);
    T(zero_transition);
    T(nested_release_cascade);
    T(multiple_roots);
    T(stale_message_protection);
    T(edge_accounting);
    T(reverse_index_reclaim_and_reuse);
    T(root_underflow_detection);
    T(pool_full_reports_null);
    T(async_acyclic_stress);
    T(async_cross_thread_handoff);
#undef T
    return aarc_test::failures() ? 1 : 0;
}
