// Suspect-accounting tests.
//
// The engine keeps a lazy suspect stack that is a *superset*: entries are never
// removed individually, so the stack holds stale and duplicate indexes that are
// filtered at scan time. That is fine for the scan, but it means stack size is
// NOT the number of pending suspects -- and the adaptive trigger reads that
// number. These tests pin the distinction down.

#include "arc/runtime.hpp"
#include "th_test.hpp"

using namespace loxis::aarc;

namespace {

// 1. Marking the same object 1000 times counts once (mark is idempotent).
void duplicate_marks_count_once() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();

    for (int k = 0; k < 1000; ++k) rt.engine().markSuspectForTest(a.index);

    CHECK_EQ(rt.engine().activeSuspects(), 1u);
    CHECK_EQ(rt.engine().suspectStackEntries(), 1u);
    CHECK_EQ(rt.engine().staleSuspectEntries(), 0u);
    rt.engine().assertActiveSuspects();
}

// 2. The real duplicate path is mark -> clear -> mark: the clear drops the mark
//    but leaves the stack entry behind, so entries pile up while the live count
//    stays correct. This is exactly the inflation the adaptive trigger used to
//    read.
void stale_entries_do_not_count() {
    Runtime rt(16, Runtime::Mode::Sync);
    auto m = rt.makeMutator();
    ObjHandle a = m->alloc();

    for (int k = 0; k < 1000; ++k) {
        rt.engine().markSuspectForTest(a.index);
        rt.engine().clearSuspectForTest(a.index);
    }

    CHECK_EQ(rt.engine().activeSuspects(), 0u);
    CHECK_EQ(rt.engine().suspectStackEntries(), 1000u);   // all stale
    CHECK_EQ(rt.engine().staleSuspectEntries(), 1000u);
    // Nothing is marked, so a scan must find nothing to do.
    CHECK_EQ(rt.engine().pendingSuspects(), 0u);

    // Re-marking adds one more entry, and the live count is 1 again.
    rt.engine().markSuspectForTest(a.index);
    CHECK_EQ(rt.engine().activeSuspects(), 1u);
    CHECK_EQ(rt.engine().suspectStackEntries(), 1001u);
    CHECK_EQ(rt.engine().staleSuspectEntries(), 1000u);
    rt.engine().assertActiveSuspects();
}

// 3. Collecting a cycle clears every mark; the counter never underflows and
//    ends at exactly zero.
void collect_leaves_no_pending_suspects() {
    Runtime rt(32, Runtime::Mode::Sync);
    auto m = rt.makeMutator();

    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));
    CHECK(m->rootRemove(a));
    CHECK(m->rootRemove(b));

    // Both members are now suspect (they still hold each other up).
    CHECK_EQ(rt.engine().activeSuspects(), 2u);
    rt.engine().assertActiveSuspects();

    rt.awaitQuiesce();

    CHECK_EQ(rt.engine().activeSuspects(), 0u);
    CHECK_EQ(rt.engine().staleSuspectEntries(), rt.engine().suspectStackEntries());
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_EQ(rt.liveCells(), 0u);
    rt.engine().assertActiveSuspects();
}

// 4. Re-marking after a scan must not double count (the real-world duplicate
//    path: a scan consumes the mark, a later removal re-marks the same slot).
void remark_after_scan_counts_once() {
    Runtime rt(32, Runtime::Mode::Sync);
    auto m = rt.makeMutator();

    ObjHandle a = m->alloc();
    ObjHandle b = m->alloc();
    CHECK(m->edgeAdd(a, b));
    CHECK(m->edgeAdd(b, a));

    // Hold the cycle up with an external root so it survives the scan.
    ObjHandle keeper = m->alloc();
    CHECK(m->edgeAdd(keeper, a));

    CHECK(m->rootRemove(a));
    CHECK(m->rootRemove(b));
    CHECK_EQ(rt.engine().activeSuspects(), 2u);

    rt.awaitQuiesce();   // scans, finds the component is externally referenced

    // The cycle is alive, so its marks were consumed by the scan; re-mark by
    // dropping the keeper.
    CHECK(m->rootRemove(keeper));
    rt.awaitQuiesce();
    CHECK_EQ(rt.engine().activeSuspects(), 0u);
    CHECK_EQ(rt.liveCells(), 0u);
    rt.engine().assertActiveSuspects();
}

} // namespace

int main() {
    std::printf("[test_suspects]\n");
    duplicate_marks_count_once();
    stale_entries_do_not_count();
    collect_leaves_no_pending_suspects();
    remark_after_scan_counts_once();
    return aarc_test::failures() ? 1 : 0;
}
