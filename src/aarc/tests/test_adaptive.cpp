// M2 (adaptive) tests: two authorities coexist over ONE shared CellPool +
// RefTable — per-owner inline LOCAL engines and the background worker's SHARED
// engine (design doc §7 M2 / M2c hard gates).
//
// What M2 proves (and these gates assert):
//   * pure-LOCAL acyclic + cyclic garbage is reclaimed by the owner's inline
//     engine at its own collectLocal() safe point — never via the worker;
//   * pre-classified SHARED garbage is reclaimed by the worker;
//   * a LOCAL thread and a SHARED thread run concurrently with zero cross-talk;
//   * touching a foreign-owner LOCAL cell, or writing a cross-regime edge, is a
//     hard refusal (promotionRequired > 0) — nothing is corrupted.
//
// Escape workloads (the events that WOULD promote) are M3, out of scope here.
// These tests run with AARC_CHECK_INVARIANTS=1 so the engine's own authority
// asserts are live.

#include "arc/runtime.hpp"
#include "th_test.hpp"

#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

// Tally live cells per regime.  Call only when every engine is idle/quiesced.
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

// Both authorities are alive over one runtime: the Runtime engine is the SHARED
// worker; an adaptive mutator carries its own LOCAL engine.
void dual_authorities_alive() {
    Runtime rt(32, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();
    CHECK(rt.engine().kind() == Regime::Shared);
    CHECK(rt.engine().mixed());
    CHECK(m->adaptive());
    CHECK(m->localEngine()->kind() == Regime::Local);
    CHECK(m->localEngine()->mixed());
}

// M2c gate 1: pure-LOCAL acyclic workload reclaimed by the owner's inline
// engine.  The worker is alive but sees nothing; nothing leaks, no refusals.
void local_acyclic_reclaims() {
    Runtime rt(2048, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();
    const int kIters = 400;               // 2 cells each; all reclaimed at the end
    for (int i = 0; i < kIters; ++i) {
        ObjHandle p = m->allocLocal();
        ObjHandle c = m->allocLocal();
        CHECK(p != kNullHandle && c != kNullHandle);
        CHECK(m->edgeAdd(p, c));          // LOCAL + mine -> inline, no refusal
        CHECK(m->rootRemove(c));          // keep c reachable via p only
        CHECK(m->rootRemove(p));
    }
    m->collectLocal();                    // owner-side safe point
    rt.awaitQuiesce();                    // quiesce the (idle) shared side too
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(m->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_EQ(rt.pool().errorCount(), 0ull);
}

// M2c gate 2: a single-owner cycle must be collected INLINE — cycles are never
// a promotion trigger (design doc §2 rule 2).
void local_cycle_reclaims_inline() {
    Runtime rt(64, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();
    ObjHandle a = m->allocLocal();
    ObjHandle b = m->allocLocal();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->rootRemove(a));
    CHECK(m->rootRemove(b));              // each now referenced only by the other
    m->collectLocal();
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(m->localEngine()->totalErrors(), 0ull);
}

// M2c gate 3: pre-classified SHARED cyclic garbage is reclaimed by the worker.
void shared_cycle_reclaimed_by_worker() {
    Runtime rt(256, Runtime::Mode::Adaptive);
    auto m = rt.makeMutator();
    ObjHandle a = m->allocShared();
    ObjHandle b = m->allocShared();
    CHECK(a != kNullHandle && b != kNullHandle);
    CHECK(rt.pool().regimeOf(a) == Regime::Shared);
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->rootRemove(a));
    CHECK(m->rootRemove(b));
    rt.awaitQuiesce();                    // worker drains + cycle-collects
    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// M2c gate 4: a LOCAL thread and a SHARED thread hammer one pool at the same
// time.  Disjoint slot ownership means zero cross-talk: each side reclaims its
// own garbage, the regime invariant holds, and no error counters move.
void local_plus_shared_concurrent_no_crosstalk() {
    Runtime rt(1u << 18, Runtime::Mode::Adaptive);
    std::unique_ptr<Mutator> mLocal, mShared;

    std::thread tLocal([&] {
        mLocal = rt.makeMutator();
        for (int i = 0; i < 16000; ++i) {
            ObjHandle p = mLocal->allocLocal();
            ObjHandle c = mLocal->allocLocal();
            if (p == kNullHandle || c == kNullHandle) continue;
            mLocal->edgeAdd(p, c);
            mLocal->rootRemove(c);
            mLocal->rootRemove(p);
        }
        mLocal->collectLocal();
    });

    std::thread tShared([&] {
        mShared = rt.makeMutator();
        for (int i = 0; i < 6000; ++i) {
            ObjHandle a = mShared->allocShared();
            ObjHandle b = mShared->allocShared();
            if (a == kNullHandle || b == kNullHandle) continue;
            mShared->edgeAdd(a, b);
            mShared->edgeAdd(b, a);
            mShared->rootRemove(a);
            mShared->rootRemove(b);
        }
    });

    tLocal.join();
    tShared.join();
    rt.awaitQuiesce();                    // settle the SHARED side

    uint32_t nl, ns;
    countByRegime(rt.pool(), nl, ns);
    CHECK_EQ(nl, 0u);
    CHECK_EQ(ns, 0u);
    CHECK_EQ(mLocal->promotionRequired(), 0ull);
    CHECK_EQ(mShared->promotionRequired(), 0ull);
    CHECK_EQ(mLocal->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(mShared->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_EQ(rt.pool().errorCount(), 0ull);
    // Whole-pool scan is safe now (all engines idle): no edge may cross regimes.
    CHECK(rt.engine().regimeInvariantOk());
}

// M2c gates 5/6 (cross-owner root ops / cross-regime edges as hard REFUSALS)
// are superseded by M3: those ops now trigger a promotion and complete.  They
// move to test_m3.cpp, which exercises the transfer (self-promotion is
// deterministic single-thread; foreign-owner escape needs a serving owner
// thread).  `promotionRequired()` is now only non-zero when a promotion cannot
// be arranged, which none of the pure-regime gates below trigger.

} // namespace

int main() {
    std::printf("[test_adaptive]\n");
    dual_authorities_alive();
    local_acyclic_reclaims();
    local_cycle_reclaims_inline();
    shared_cycle_reclaimed_by_worker();
    local_plus_shared_concurrent_no_crosstalk();
    return aarc_test::failures() ? 1 : 0;
}
