// M3 tests: ownership-transfer promotion (design doc §6 + §7 M3).
//
// M2 proved two authorities coexist with zero cross-talk.  M3 moves authority:
// when a reference crosses an owner's LOCAL boundary, the LOCAL component is
// handed to the SHARED worker as one undirected unit, so no cross-regime edge
// survives and the worker reclaims whatever becomes garbage.
//
// What these kill-criteria gates assert:
//   * pure-LOCAL workloads never promote (cycles are not a promotion trigger);
//   * a LOCAL object referencing a SHARED one self-promotes its component
//     (deterministic, single-thread);
//   * a SHARED object referencing a LOCAL one promotes that LOCAL component;
//   * a foreign thread rooting a LOCAL object hands off through its owner
//     (threaded), handles stay stable, and the worker reclaims the escape;
//   * two threads racing to escape the same cell collapse onto one hand-off.
//   * at every stable point: no cross-regime edge, no leak, no error.
//
// Run with AARC_CHECK_INVARIANTS=1 so the engine's own authority asserts are
// live (wired in CMakeLists; test_stress is the exception).

#include "arc/runtime.hpp"
#include "th_test.hpp"

#include <atomic>
#include <thread>

using namespace loxis::aarc;

namespace {

void countByRegime(CellPool& pool, uint32_t& local, uint32_t& shared) {
    local = 0;
    shared = 0;
    pool.forEachLive([&](ObjHandle h) {
        if (pool.regimeOf(h) == Regime::Local)
            ++local;
        else
            ++shared;
    });
}

// Gate: a pure-LOCAL acyclic+cyclic workload performed by one owner must never
// promote (cycles are collected inline; promotionsDone stays 0).
void pure_local_never_promotes() {
    Runtime rt(512, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();
    for (int i = 0; i < 60; ++i) {
        ObjHandle a = m->allocLocal();
        ObjHandle b = m->allocLocal();
        m->edgeAdd(a, b);
        m->edgeAdd(b, a);                       // cycle
        m->rootRemove(a);
        m->rootRemove(b);
    }
    m->collectLocal();
    rt.awaitQuiesce();
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(rt.promotionsDone(), 0ull);        // nothing escaped
    CHECK_EQ(m->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Gate: a LOCAL object gaining an edge to a SHARED object forces its OWN
// component SHARED (a LOCAL cell may not hold a cross-regime edge).  Self-
// promotion runs synchronously on the owner thread; handles stay stable.
void local_edge_to_shared_self_promotes() {
    Runtime rt(512, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();

    ObjHandle x = m->allocLocal();
    ObjHandle y = m->allocLocal();
    CHECK(m->edgeAdd(x, y));                     // x -> y, both LOCAL(me)
    ObjHandle s = m->allocShared();
    const uint32_t gx = x.gen, ix = x.index;

    CHECK(m->edgeAdd(x, s));                     // LOCAL src -> SHARED dst
    CHECK_EQ(m->promotionRequired(), 0ull);      // promotion, not a refusal
    CHECK_EQ(rt.promotionsDone(), 1ull);

    // The whole undirected component of x (x and its child y) moved to SHARED;
    // x's identity (index + generation) is untouched.
    CHECK(rt.pool().valid(x));
    CHECK_EQ(x.gen, gx);
    CHECK_EQ(x.index, ix);
    CHECK(rt.pool().regimeOf(x) == Regime::Shared);
    CHECK(rt.pool().regimeOf(y) == Regime::Shared);

    // Drop the roots through the worker now that everything is SHARED.
    CHECK(m->rootRemove(x));
    CHECK(m->rootRemove(y));
    CHECK(m->rootRemove(s));
    rt.awaitQuiesce();
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(m->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK(rt.engine().regimeInvariantOk());
}

// Gate: a SHARED object referencing a LOCAL one promotes that LOCAL component
// so the edge becomes SHARED->SHARED.  Also single-thread deterministic.
void shared_edge_to_local_promotes_local() {
    Runtime rt(512, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();

    ObjHandle s = m->allocShared();
    ObjHandle x = m->allocLocal();
    const uint32_t gx = x.gen;

    CHECK(m->edgeAdd(s, x));                     // SHARED src -> LOCAL dst
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(rt.promotionsDone(), 1ull);
    CHECK(rt.pool().valid(x));
    CHECK_EQ(x.gen, gx);
    CHECK(rt.pool().regimeOf(x) == Regime::Shared);

    CHECK(m->rootRemove(s));
    CHECK(m->rootRemove(x));
    rt.awaitQuiesce();
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(m->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Gate: a foreign thread rooting a LOCAL cell (safe cross-thread handoff).  The
// requesting mutator blocks; the owner serves at its polled safe point; the
// whole LOCAL chain x -> y is handed to the worker and reclaimed once both the
// original owner and the new root drop it.  Handles stay valid throughout.
void foreign_owner_handoff_reclaimed() {
    Runtime rt(1u << 14, Runtime::Mode::Adaptive);

    ObjHandle sharedX;                       // published under `ready` HB
    std::atomic<bool> ready{false};
    std::atomic<bool> uDone{false};
    std::atomic<bool> uOk{false};
    std::unique_ptr<Mutator> mT, mU;

    std::thread tOwner([&] {
        mT = rt.makeMutator();
        ObjHandle x = mT->allocLocal();
        ObjHandle y = mT->allocLocal();
        mT->edgeAdd(x, y);                    // x -> y, both LOCAL(owner T)
        sharedX = x;
        ready.store(true, std::memory_order_release);
        // Serve promotion requests at our local safe point until the escaping
        // thread has taken its root (keeps x alive via OUR root meanwhile).
        while (!uDone.load(std::memory_order_acquire)) {
            mT->pollPromotions();
            std::this_thread::yield();
        }
        // x (and y, promoted with it) are SHARED now; drop our roots via worker.
        mT->rootRemove(x);
        mT->rootRemove(y);
    });

    std::thread tEscaper([&] {
        mU = rt.makeMutator();
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        const ObjHandle x = sharedX;
        uOk.store(mU->rootAdd(x), std::memory_order_relaxed);    // blocks -> promote
        uOk.store(uOk.load(std::memory_order_relaxed) &&
                      mU->rootRemove(x), std::memory_order_relaxed);
        uDone.store(true, std::memory_order_release);
    });

    tOwner.join();
    tEscaper.join();
    rt.awaitQuiesce();

    CHECK(uOk.load(std::memory_order_relaxed));
    CHECK_EQ(rt.promotionsDone(), 1ull);
    CHECK_EQ(mT->promotionRequired(), 0ull);
    CHECK_EQ(mU->promotionRequired(), 0ull);
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(mT->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(mU->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_EQ(rt.pool().errorCount(), 0ull);
    CHECK(rt.engine().regimeInvariantOk());
}

// Gate: two threads race to root the same LOCAL cell of a third owner.  Both
// root-adds land (the RefTable row carries the original root plus both new
// roots), the hand-off collapses onto one (or at most a couple of) promotion
// rounds, and the worker reclaims everything once all three roots drop.
void concurrent_double_escape_collapses() {
    Runtime rt(1u << 14, Runtime::Mode::Adaptive);

    ObjHandle sharedX;
    std::atomic<bool> ready{false};
    std::atomic<int> added{0};
    std::atomic<bool> goDrop{false};
    std::atomic<int> dropped{0};
    std::atomic<bool> u1Ok{true}, u2Ok{true};
    std::unique_ptr<Mutator> mT, mU1, mU2;

    std::thread tOwner([&] {
        mT = rt.makeMutator();
        ObjHandle x = mT->allocLocal();
        ObjHandle y = mT->allocLocal();
        mT->edgeAdd(x, y);
        sharedX = x;
        ready.store(true, std::memory_order_release);
        while (added.load(std::memory_order_acquire) < 2)
            mT->pollPromotions();
        // Both escapers rooted x.  Wait for the signal, then drop our roots.
        while (!goDrop.load(std::memory_order_acquire)) {
            mT->pollPromotions();
            std::this_thread::yield();
        }
        mT->rootRemove(x);
        mT->rootRemove(y);
    });

    auto escaper = [&](std::unique_ptr<Mutator>& mU, std::atomic<bool>& ok) {
        mU = rt.makeMutator();
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        const ObjHandle x = sharedX;
        ok.store(mU->rootAdd(x), std::memory_order_relaxed);   // blocks -> promote
        added.fetch_add(1, std::memory_order_acq_rel);
        while (!goDrop.load(std::memory_order_acquire)) std::this_thread::yield();
        ok.store(ok.load(std::memory_order_relaxed) &&
                     mU->rootRemove(x), std::memory_order_relaxed);
        dropped.fetch_add(1, std::memory_order_acq_rel);
    };
    std::thread tE1(escaper, std::ref(mU1), std::ref(u1Ok));
    std::thread tE2(escaper, std::ref(mU2), std::ref(u2Ok));

    // Once both escapers report success, drain so both RootAdds land, verify the
    // shared row actually carries all three roots, then release the drops.
    while (added.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    rt.awaitQuiesce();
    CHECK(u1Ok.load(std::memory_order_relaxed));
    CHECK(u2Ok.load(std::memory_order_relaxed));
    CHECK_EQ(rt.pool().regimeOf(sharedX), Regime::Shared);
    CHECK_EQ(rt.engine().refcountOf(sharedX), 3);   // T root + two escapers
    goDrop.store(true, std::memory_order_release);

    tOwner.join();
    while (dropped.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    tE1.join();
    tE2.join();
    rt.awaitQuiesce();

    CHECK_EQ(mT->promotionRequired(), 0ull);
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    // Collapsed: likely exactly one hand-off; allow a second if the racers were
    // served in separate rounds.
    CHECK_MSG(rt.promotionsDone() >= 1 && rt.promotionsDone() <= 2,
              "double escape collapsed to few promotion rounds");
    CHECK_EQ(mT->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_EQ(rt.pool().errorCount(), 0ull);
    CHECK(rt.engine().regimeInvariantOk());
}

// Gate: many sequential hand-offs through one owner + one escaper.  Exercises
// the promote/serve/worker path repeatedly (epoch monotonicity, gate cleanup,
// no leak / double-free across a run of transfers) rather than a single event.
void many_sequential_handoffs() {
    constexpr int kIters = 40;
    Runtime rt(1u << 12, Runtime::Mode::Adaptive);

    std::atomic<bool> ready[kIters];
    std::atomic<bool> ack[kIters];
    for (int k = 0; k < kIters; ++k) {
        ready[k].store(false, std::memory_order_relaxed);
        ack[k].store(false, std::memory_order_relaxed);
    }
    ObjHandle sharedX[kIters];
    std::atomic<int> escFail{0};
    std::unique_ptr<Mutator> mT, mU;

    std::thread tOwner([&] {
        mT = rt.makeMutator();
        for (int k = 0; k < kIters; ++k) {
            ObjHandle x = mT->allocLocal();
            ObjHandle y = mT->allocLocal();
            if (x == kNullHandle || y == kNullHandle) continue;
            mT->edgeAdd(x, y);                 // x -> y, both LOCAL(owner T)
            sharedX[k] = x;
            ready[k].store(true, std::memory_order_release);
            while (!ack[k].load(std::memory_order_acquire))
                mT->pollPromotions();          // serve the escaping thread
            // Escaper rooted (and dropped) x; x,y are SHARED now — drop ours.
            mT->rootRemove(x);
            mT->rootRemove(y);
            if (k % 5 == 0) mT->collectLocal();
        }
    });

    std::thread tEscaper([&] {
        mU = rt.makeMutator();
        for (int k = 0; k < kIters; ++k) {
            while (!ready[k].load(std::memory_order_acquire)) std::this_thread::yield();
            const ObjHandle x = sharedX[k];
            if (!mU->rootAdd(x)) escFail.fetch_add(1, std::memory_order_relaxed);  // promote
            if (!mU->rootRemove(x)) escFail.fetch_add(1, std::memory_order_relaxed);
            ack[k].store(true, std::memory_order_release);
        }
    });

    tOwner.join();
    tEscaper.join();
    rt.awaitQuiesce();
    rt.pool().drainRecycleAll();               // settle any pile leftovers

    CHECK_EQ(escFail.load(std::memory_order_relaxed), 0);
    CHECK_EQ(rt.promotionsDone(), static_cast<uint64_t>(kIters));
    CHECK_EQ(mT->promotionRequired(), 0ull);
    CHECK_EQ(mU->promotionRequired(), 0ull);
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(mT->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(mU->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_EQ(rt.pool().errorCount(), 0ull);
    CHECK(rt.engine().regimeInvariantOk());
}

} // namespace

int main() {
    std::printf("[test_m3]\n");
    std::printf("  pure_local...\n"); std::fflush(stdout);
    pure_local_never_promotes();
    std::printf("  self_promote...\n"); std::fflush(stdout);
    local_edge_to_shared_self_promotes();
    std::printf("  shared_to_local...\n"); std::fflush(stdout);
    shared_edge_to_local_promotes_local();
    std::printf("  foreign_handoff...\n"); std::fflush(stdout);
    foreign_owner_handoff_reclaimed();
    std::printf("  double_escape...\n"); std::fflush(stdout);
    concurrent_double_escape_collapses();
    std::printf("  many_sequential...\n"); std::fflush(stdout);
    many_sequential_handoffs();
    return aarc_test::failures() ? 1 : 0;
}
