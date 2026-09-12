// M10a batch-transport regressions.
//
// The producer stages messages in its ProducerSlot and publishes the block with
// one publication window / epoch bump / wake.  These tests pin the boundaries
// where that staging is observable:
//   * a quiesce must flush the caller's staged batch (else it can never take a
//     stable cut and hangs);
//   * owner close must flush (nothing may be lost);
//   * a promotion serve must flush (the worker's view must be current before
//     authority is handed over);
//   * a stale generation staged next to a live one must still be rejected;
//   * a full ring during a batch flush must not lose messages or deadlock;
//   * two mutators flushing concurrently still produce a consistent cut;
//   * batching is observationally equivalent to batch=1 (differential test on a
//     deterministic op stream: same live set, refcounts and edges).
//
// Runs in both edge schemes (reverse default / AARC_CONSERVATIVE).

#include "arc/runtime.hpp"
#include "th_test.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <tuple>
#include <cstdlib>
#include <vector>

using namespace loxis::aarc;

namespace {

// ---- quiesce with a partial batch ----------------------------------------
// 3 messages staged (limit 32), then a quiesce from the same thread.  The batch
// must be published and applied; nothing may leak.
void quiesce_flushes_partial_batch() {
    Runtime rt(256, Runtime::Mode::Async, true, 32);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();
    CHECK(h != kNullHandle);
    CHECK(m->rootAdd(h));
    CHECK(m->rootRemove(h));
    CHECK(m->rootRemove(h));                 // back to zero roots
    // still staged: 4 messages, well below the limit
    CHECK_EQ(rt.msgsPublished(), 0ull);

    rt.awaitQuiesce();
    CHECK(rt.msgsPublished() >= 4ull);
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// ---- close with a partial batch ------------------------------------------
// The mutator stages work and closes; close must publish the staged block.
void close_flushes_partial_batch() {
    Runtime rt(256, Runtime::Mode::Async, true, 32);
    {
        auto m = rt.makeMutator();
        ObjHandle h = m->alloc();
        CHECK(h != kNullHandle);
        CHECK(m->rootRemove(h));
        CHECK_EQ(rt.msgsPublished(), 0ull);   // staged, not published
        m->close();                           // must flush + drain
    }
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// ---- promotion with a partial batch --------------------------------------
// A LOCAL cell escapes while shared-side ops are still staged: the serve path
// must publish them first so the worker's cut is current.
void promotion_flushes_partial_batch() {
    Runtime rt(512, Runtime::Mode::Adaptive, true, 32);
    auto m = rt.makeMutator();

    ObjHandle s = m->allocShared();          // staged RootAdd
    ObjHandle x = m->allocLocal();
    ObjHandle y = m->allocLocal();
    CHECK(s != kNullHandle && x != kNullHandle && y != kNullHandle);
    CHECK(m->edgeAdd(x, y));                 // LOCAL->LOCAL, inline
    CHECK_EQ(rt.msgsPublished(), 0ull);      // shared-side RootAdd still staged

    CHECK(m->edgeAdd(x, s));                 // escapes -> promotion
    CHECK_EQ(m->promotionRequired(), 0ull);
    CHECK_EQ(rt.promotionsDone(), 1ull);
    CHECK(rt.pool().regimeOf(x) == Regime::Shared);
    CHECK(rt.pool().regimeOf(y) == Regime::Shared);

    CHECK(m->rootRemove(s));
    CHECK(m->rootRemove(x));
    CHECK(m->rootRemove(y));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(m->localEngine()->totalErrors(), 0ull);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// ---- stale generation next to a live one --------------------------------
// Generation validation stays PER MESSAGE: a stale message must be dropped
// without poisoning the live messages published in the same / neighbouring
// blocks.  (A stale message cannot be staged through the mutator API -- it
// rejects dead handles -- so it is injected at the engine, exactly like
// test_arc's stale_message_protection.)
void stale_generation_between_batches() {
    Runtime rt(2, Runtime::Mode::Async, true, 32);   // 2 slots => forced reuse
    auto m = rt.makeMutator();

    ObjHandle old = m->alloc();
    ObjHandle keep = m->alloc();
    CHECK(old != kNullHandle && keep != kNullHandle);
    CHECK(m->rootRemove(old));               // staged block
    rt.awaitQuiesce();                       // applied: `old` is garbage
    CHECK(!rt.pool().valid(old));

    ObjHandle fresh = m->alloc();            // reuses old.index, new generation
    CHECK_EQ(fresh.index, old.index);
    CHECK(fresh.gen != old.gen);
    m->flush();                              // publish the staged RootAdd(fresh)
    rt.awaitQuiesce();                       // ...and let the worker apply it

    // Stale messages for the old generation land between two live blocks.
    const uint64_t staleBefore = rt.engine().staleDropped();
    rt.engine().process({GCOp::RootAdd, kNullHandle, old});
    rt.engine().process({GCOp::EdgeRemove, old, keep});
    CHECK_EQ(rt.engine().staleDropped(), staleBefore + 2);
    CHECK_EQ(rt.engine().refcountOf(fresh), 1);   // the live block applied
    CHECK_EQ(rt.engine().refcountOf(keep), 1);

    CHECK(m->rootRemove(fresh));
    CHECK(m->rootRemove(keep));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    // exactly the two injected stale messages were dropped, nothing else
    CHECK_EQ(rt.engine().totalErrors(), 2ull);
}

// ---- full ring during a batch flush --------------------------------------
// The worker is parked so the ring cannot drain; the producer must spin (not
// drop, not deadlock) until it can publish the whole block.
struct Gate {
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;
    bool reached = false;
    void signalAndWait() {
        std::unique_lock<std::mutex> lk(mu);
        reached = true;
        cv.notify_all();
        cv.wait(lk, [&] { return open; });
    }
    void waitReached() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return reached; });
    }
    void release() {
        std::lock_guard<std::mutex> lk(mu);
        open = true;
        cv.notify_all();
    }
};

void queue_full_batch_flush_no_loss() {
    Runtime rt(1u << 15, Runtime::Mode::Async, true, 256);
    auto m = rt.makeMutator();

    Gate roundGate;
    Runtime::TestHooks hooks;
    hooks.afterRound = [&] { roundGate.signalAndWait(); };
    rt.setTestHooks(std::move(hooks));

    ObjHandle first = m->alloc();
    CHECK(first != kNullHandle);
    m->flush();                              // wake the worker; it parks after 1 round
    roundGate.waitReached();

    // The worker is parked, so the ring can never drain: pushing more than
    // kQueueCapacity messages MUST make the producer spin, never lose one.
    constexpr int kExtra = 6000;             // > kQueueCapacity (4096)
    std::thread releaser([&] {
        for (int i = 0; i < 4000 && rt.spinCount() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        roundGate.release();
    });

    std::vector<ObjHandle> hs;
    hs.reserve(kExtra);
    for (int i = 0; i < kExtra; ++i) {
        ObjHandle h = m->alloc();
        CHECK(h != kNullHandle);
        hs.push_back(h);
    }
    releaser.join();
    CHECK_MSG(rt.spinCount() > 0, "producer had to spin on the full ring");

    for (ObjHandle h : hs) CHECK(m->rootRemove(h));
    CHECK(m->rootRemove(first));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// ---- two mutators flushing concurrently ----------------------------------
// Both producers stage and publish blocks while the worker drains; after a full
// quiesce nothing may leak and no error counter may move.
void two_mutators_flush_concurrently() {
    Runtime rt(1u << 18, Runtime::Mode::Async, true, 64);
    std::atomic<long long> objs{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 2; ++t) {
        ts.emplace_back([&] {
            auto m = rt.makeMutator();
            for (int i = 0; i < 20000; ++i) {
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
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    CHECK_MSG(rt.batchesPublished() > 0, "batching actually published blocks");
}

// ---- differential: batch=1 vs batch=N on one deterministic op stream ------
struct Snap {
    // id -> (refcount, sorted ids of outgoing targets)
    std::vector<std::tuple<uint32_t, int64_t, std::vector<uint32_t>>> cells;
    bool operator==(const Snap& o) const { return cells == o.cells; }
};

Snap snapshot(Runtime& rt) {
    Snap s;
    rt.pool().forEachLive([&](ObjHandle h) {
        std::vector<uint32_t> outs;
        for (const ObjHandle& t : rt.engine().table().outs(h.index))
            if (rt.pool().valid(t)) outs.push_back(rt.pool().getId(t));
        std::sort(outs.begin(), outs.end());
        s.cells.emplace_back(rt.pool().getId(h), rt.engine().refcountOf(h),
                             std::move(outs));
    });
    std::sort(s.cells.begin(), s.cells.end());
    return s;
}

// Deterministic valid op stream: every allocated cell keeps at least one root
// until the final release, so the model never touches a reclaimed handle.
// Returns the final snapshot taken at a quiesce point.
Snap runStream(uint32_t batchSize, bool fold, uint32_t seed, int ops,
               uint32_t& nextId) {
    Runtime rt(1u << 14, Runtime::Mode::Async, true, batchSize, fold);
    auto m = rt.makeMutator();
    nextId = 1;

    struct Node { ObjHandle h; int roots; std::vector<ObjHandle> outs; };
    std::vector<Node> live;
    uint64_t s = seed;
    auto rnd = [&] { s = s * 6364136223846793005ull + 1442695040888963407ull;
                     return uint32_t(s >> 33); };

    for (int i = 0; i < ops; ++i) {
        const uint32_t op = rnd() % 5;
        if (live.empty() || op == 0) {
            if (live.size() >= 400) continue;
            ObjHandle h = m->alloc();
            if (h == kNullHandle) continue;
            rt.pool().setId(h, nextId++);
            live.push_back({h, 1, {}});
            continue;
        }
        Node& a = live[rnd() % live.size()];
        switch (op) {
            case 1:                          // extra root
                if (a.roots < 4 && m->rootAdd(a.h)) ++a.roots;
                break;
            case 2:                          // drop an extra root
                if (a.roots > 1 && m->rootRemove(a.h)) --a.roots;
                break;
            case 3: {                        // add an edge a -> b
                if (a.outs.size() >= 3) break;
                Node& b = live[rnd() % live.size()];
                if (m->edgeAdd(a.h, b.h)) a.outs.push_back(b.h);
                break;
            }
            default: {                       // remove an existing edge
                if (a.outs.empty()) break;
                const size_t k = rnd() % a.outs.size();
                if (m->edgeRemove(a.h, a.outs[k]))
                    a.outs.erase(a.outs.begin() + static_cast<ptrdiff_t>(k));
                break;
            }
        }
    }
    rt.awaitQuiesce();
    Snap snap = snapshot(rt);
    // Drop every root: the random edge stream can build cycles, so this also
    // asserts the cycle collector reclaims everything at the final quiesce.
    for (Node& n : live)
        for (int r = 0; r < n.roots; ++r) m->rootRemove(n.h);
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
    return snap;
}

void batching_equals_unbatched_stream(int ops, const uint32_t* seeds, size_t nseeds) {
    for (size_t si = 0; si < nseeds; ++si) {
        const uint32_t seed = seeds[si];
        uint32_t id1 = 0, id2 = 0, id3 = 0, id4 = 0, id5 = 0;
        const Snap a = runStream(1, false, seed, ops, id1);
        const Snap b = runStream(16, false, seed, ops, id2);
        const Snap c = runStream(64, false, seed, ops, id3);
        const Snap d = runStream(16, true, seed, ops, id4);
        const Snap e = runStream(64, true, seed, ops, id5);
        CHECK_EQ(id1, id2);
        CHECK_EQ(id1, id3);
        CHECK_EQ(id1, id4);
        CHECK_EQ(id1, id5);
        CHECK_MSG(a == b, "batch=16 final graph equals batch=1");
        CHECK_MSG(a == c, "batch=64 final graph equals batch=1");
        CHECK_MSG(a == d, "batch=16+fold final graph equals batch=1");
        CHECK_MSG(a == e, "batch=64+fold final graph equals batch=1");
    }
}

// ---- M10b: folding -------------------------------------------------------

// A balanced RootAdd/RootRemove pair folds to ONE RootCheck, and that check
// must still trigger reclamation (this is the case a naive "cancel the pair"
// fold would leak).
void fold_balanced_pair_still_reclaims() {
    Runtime rt(256, Runtime::Mode::Async, true, 32, /*fold=*/true);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();                // RootAdd
    CHECK(h != kNullHandle);
    CHECK(m->rootRemove(h));                 // RootRemove -> net zero
    rt.awaitQuiesce();

    const Runtime::FoldStats s = rt.foldStats();
    CHECK_EQ(s.in[0], 1ull);                 // one RootAdd staged
    CHECK_EQ(s.in[1], 1ull);                 // one RootRemove staged
    CHECK_EQ(s.out[2], 1ull);                // emitted as a single RootCheck
    CHECK_EQ(s.outTotal(), 1ull);
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Net deltas are applied with their multiplicity.
void fold_applies_net_delta() {
    Runtime rt(256, Runtime::Mode::Async, true, 32, /*fold=*/true);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();                // +1
    CHECK(h != kNullHandle);
    CHECK(m->rootAdd(h));                    // +1
    CHECK(m->rootAdd(h));                    // +1
    CHECK(m->rootRemove(h));                 // -1  => net +2
    rt.awaitQuiesce();
    CHECK_EQ(rt.engine().refcountOf(h), 2);
    CHECK_EQ(rt.foldStats().outTotal(), 1ull);   // one counted RootAdd

    CHECK(m->rootRemove(h));
    CHECK(m->rootRemove(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Edge ops are never folded (v1 folds root deltas only) and survive intact.
void fold_leaves_edge_ops_alone() {
    Runtime rt(256, Runtime::Mode::Async, true, 32, /*fold=*/true);
    auto m = rt.makeMutator();
    ObjHandle p = m->alloc();
    ObjHandle c = m->alloc();
    CHECK(p != kNullHandle && c != kNullHandle);
    CHECK(m->edgeAdd(p, c));
    CHECK(m->edgeRemove(p, c));
    CHECK(m->rootRemove(c));
    CHECK(m->rootRemove(p));
    rt.awaitQuiesce();

    const Runtime::FoldStats s = rt.foldStats();
    CHECK_EQ(s.in[3], 1ull);                 // EdgeAdd staged
    CHECK_EQ(s.in[4], 1ull);                 // EdgeRemove staged
    CHECK_EQ(s.out[3], 1ull);                // both emitted verbatim
    CHECK_EQ(s.out[4], 1ull);
    CHECK_EQ(s.in[0], 2ull);                 // two RootAdds staged
    CHECK_EQ(s.out[2], 2ull);                // two balanced pairs -> RootCheck
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// Folding must not lose messages across a flush barrier: a pair split over two
// batches (forced by the limit) still nets out correctly.
void fold_does_not_cross_batches() {
    Runtime rt(256, Runtime::Mode::Async, true, 2, /*fold=*/true);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();                // batch 1: RootAdd
    CHECK(h != kNullHandle);
    CHECK(m->rootAdd(h));                    // batch 1: RootAdd (full -> flush)
    CHECK(m->rootRemove(h));                 // batch 2: RootRemove
    CHECK(m->rootRemove(h));                 // batch 2: RootRemove (full -> flush)
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

// ---- M12: flush-before-park ---------------------------------------------

// A live mutator holding a staged (unpublished) batch blocks EVERY stable cut:
// no other thread can complete a quiesce while it is parked.  This is the
// progress bug the flush-before-park contract rules out.
void park_with_staged_batch_blocks_quiesce() {
    Runtime rt(256, Runtime::Mode::Async, true, 32);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();                 // staged, NOT published
    CHECK(h != kNullHandle);
    CHECK_EQ(rt.msgsPublished(), 0ull);

    std::atomic<bool> quiesced{false};
    std::thread q([&] { rt.awaitQuiesce(); quiesced.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_MSG(!quiesced.load(), "no stable cut can exist while a batch is staged");

    m->willPark();                            // publish before parking
    CHECK(rt.msgsPublished() >= 1ull);
    q.join();
    CHECK(quiesced.load());
    CHECK_EQ(rt.liveCells(), 1u);             // h is still rooted
    CHECK(m->rootRemove(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
}

// A mutator that follows the contract never blocks another thread: willPark()
// publishes first, so the quiesce completes immediately.
void willPark_publishes_before_blocking() {
    Runtime rt(256, Runtime::Mode::Async, true, 32);
    auto m = rt.makeMutator();
    ObjHandle h = m->alloc();
    CHECK_EQ(rt.msgsPublished(), 0ull);
    m->willPark();
    CHECK(rt.msgsPublished() >= 1ull);
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 1u);
    CHECK(m->rootRemove(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.quiesceStalls(), 0ull);       // contract never violated
}

// ---- M12b: RootBag-implication fast path ---------------------------------
// The fast path drops the pool validity probe because a RootBag key proves the
// mutator added a root for exactly this (index, generation).  The generation is
// part of the key, so a handle whose slot was recycled must still be rejected.
void fast_root_valid_rejects_stale_generation() {
    Runtime rt(2, Runtime::Mode::Async, true, 32);   // 2 slots => forced reuse
    rt.setFastRootValid(true);
    auto m = rt.makeMutator();

    ObjHandle old = m->alloc();
    ObjHandle keep = m->alloc();
    CHECK(old != kNullHandle && keep != kNullHandle);
    CHECK(m->rootRemove(old));
    rt.awaitQuiesce();
    CHECK(!rt.pool().valid(old));

    ObjHandle fresh = m->alloc();            // same slot, new generation
    CHECK_EQ(fresh.index, old.index);
    CHECK(fresh.gen != old.gen);

    // Stale handle: not in the bag (generation differs) => must be refused, and
    // nothing may be published for it.
    const uint64_t published = rt.msgsPublished();
    CHECK(!m->rootRemove(old));
    CHECK_EQ(rt.msgsPublished(), published);
    // Live handle still works.
    CHECK(m->rootRemove(fresh));

    CHECK(m->rootRemove(keep));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
    CHECK_EQ(rt.engine().totalErrors(), 0ull);
}

} // namespace

int main(int argc, char** argv) {
    const int ops = argc > 1 ? std::atoi(argv[1]) : 4000;
    const uint32_t seeds[] = {1u, 7u, 99u};
    std::printf("[test_batch] ops=%d\n", ops);
#define T(f) do { std::printf("  %s...\n", #f); std::fflush(stdout); f(); } while (0)
    T(quiesce_flushes_partial_batch);
    T(close_flushes_partial_batch);
    T(promotion_flushes_partial_batch);
    T(stale_generation_between_batches);
    T(queue_full_batch_flush_no_loss);
    T(two_mutators_flush_concurrently);
    T(fold_balanced_pair_still_reclaims);
    T(fold_applies_net_delta);
    T(fold_leaves_edge_ops_alone);
    T(fold_does_not_cross_batches);
    T(willPark_publishes_before_blocking);
    T(park_with_staged_batch_blocks_quiesce);
    T(fast_root_valid_rejects_stale_generation);
    batching_equals_unbatched_stream(ops, seeds, 3);
#undef T
    return aarc_test::failures() ? 1 : 0;
}
