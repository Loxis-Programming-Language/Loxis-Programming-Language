// Deterministic concurrency protocol regressions.
//
// These litmus tests pin the protocol to specific linearization points using
// runtime test hooks (AARC_TEST_HOOKS, this target only).  They reproduce the
// races that a stress test could only hit probabilistically:
//   * a cross-producer root transfer in a worker cut window must invalidate the
//     cut (no reclaim of a live object);
//   * a message published between queue-push and publication-close must still
//     drain without granting reclaim;
//   * an owner exiting with surviving LOCAL cells hands them to the worker and
//     unblocks a blocked foreign requester;
//   * a later quiesce ticket is never completed by an earlier cut;
//   * shutdown from another thread must wake an idle worker.
#include "arc/runtime.hpp"
#include "th_test.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace loxis::aarc;

namespace {

// One-shot barrier used to park the worker (or a producer) at a hook.
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

// 64 simultaneous owner leases must not alias; the 65th acquire fails instead
// of reusing an in-flight owner id.
void owner_capacity_does_not_alias() {
    Runtime rt(256, Runtime::Mode::Adaptive);
    std::vector<std::unique_ptr<Mutator>> owners;
    owners.reserve(CellPool::kMaxOwners);
    for (uint32_t i = 0; i < CellPool::kMaxOwners; ++i)
        owners.push_back(rt.makeMutator());
    CHECK_EQ(owners.size(), static_cast<size_t>(CellPool::kMaxOwners));
    for (const auto& m : owners) CHECK(m != nullptr);
    auto overflow = rt.makeMutator();
    CHECK(overflow == nullptr);
}

// A RootAdd on producer B followed (via a synchronizes-with edge) by RootRemoves
// on producer A must never be cut so the drain sees the removes but not the add.
// The worker samples every producer's pre sequence before tails; the cross-queue
// transfer lands inside the window, so the cut must be invalid (no sweep) and
// the object must survive with exactly B's root remaining.
void consistent_cut_rejects_cross_queue_transfer() {
    // batch=1: this pins the PER-MESSAGE publication window.  (With batching the
    // same property is covered by the staged-batch tests in test_batch.cpp.)
    Runtime rt(256, Runtime::Mode::Async, true, 1);
    auto a = rt.makeMutator();
    auto b = rt.makeMutator();
    CHECK(a != nullptr && b != nullptr);

    ObjHandle h = a->alloc();
    CHECK(h != kNullHandle);
    CHECK(a->rootAdd(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.engine().refcountOf(h), 2);

    Gate preGate;
    std::atomic<int> preCount{0};
    Runtime::TestHooks hooks;
    hooks.afterProducerPre = [&](size_t) {
        if (preCount.fetch_add(1) + 1 == 2) preGate.signalAndWait();
    };
    rt.setTestHooks(std::move(hooks));

    std::thread q([&] { rt.awaitQuiesce(); });
    preGate.waitReached();

    // Both producers' pre sequences are sampled; publish the transfer now so it
    // lands before the worker reads post sequences and tails.
    CHECK(b->rootAdd(h));
    CHECK(a->rootRemove(h));
    CHECK(a->rootRemove(h));
    preGate.release();
    q.join();

    CHECK(rt.invalidCuts() > 0);
    CHECK(rt.pool().valid(h));
    CHECK_EQ(rt.engine().refcountOf(h), 1);

    CHECK(b->rootRemove(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
}

// A producer parked between pushing a message and closing its publication
// sequence holds pubSeq odd.  The worker must still drain the pushed message
// (freeing backpressure) but must not reclaim, and the ticket must wait for a
// later stable cut.
void publication_window_drains_without_reclaim() {
    // batch=1: the hook parks the producer between push and publication close,
    // which is exactly the per-message window this test defines.
    Runtime rt(256, Runtime::Mode::Async, true, 1);
    auto m = rt.makeMutator();
    CHECK(m != nullptr);

    Gate pushGate;
    Gate roundGate;
    Runtime::TestHooks hooks;
    hooks.afterQueuePush = [&] { pushGate.signalAndWait(); };
    hooks.afterRound = [&] { roundGate.signalAndWait(); };
    rt.setTestHooks(std::move(hooks));

    std::atomic<bool> allocDone{false};
    ObjHandle h;
    std::thread producer([&] {
        h = m->alloc();
        allocDone.store(true, std::memory_order_release);
    });
    pushGate.waitReached();

    std::thread waiter([&] { rt.awaitQuiesce(); });
    roundGate.waitReached();
    CHECK(!allocDone.load(std::memory_order_acquire));
    CHECK(rt.invalidCuts() > 0);

    roundGate.release();
    pushGate.release();
    producer.join();
    waiter.join();
    CHECK(h != kNullHandle);
    CHECK(rt.pool().valid(h));

    CHECK(m->rootRemove(h));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
}

// An owner with a surviving LOCAL cell closes while a foreign thread is blocked
// rooting that cell.  The owner's close must hand the cell to the worker, apply
// the reserved foreign root before/at the hand-off, and unblock the requester;
// once all roots drop, nothing leaks.
void owner_exit_hands_off_survivor() {
    Runtime rt(1024, Runtime::Mode::Adaptive);
    std::atomic<bool> ready{false};
    std::atomic<bool> ownerClosed{false};
    std::atomic<bool> requesterOk{false};
    ObjHandle shared{};
    OwnerToken ownerToken;

    std::thread owner([&] {
        auto m = rt.makeMutator();
        CHECK(m != nullptr);
        ownerToken = m->ownerToken();
        shared = m->allocLocal();
        CHECK(shared != kNullHandle);
        ready.store(true, std::memory_order_release);
        while (!rt.promotePending(ownerToken)) std::this_thread::yield();
        m->close();
        ownerClosed.store(true, std::memory_order_release);
    });

    std::thread requester([&] {
        while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        auto m = rt.makeMutator();
        CHECK(m != nullptr);
        requesterOk.store(m->rootAdd(shared), std::memory_order_release);
        if (requesterOk.load(std::memory_order_acquire))
            requesterOk.store(m->rootRemove(shared), std::memory_order_release);
    });

    owner.join();
    requester.join();
    CHECK(ownerClosed.load(std::memory_order_acquire));
    CHECK(requesterOk.load(std::memory_order_acquire));
    rt.awaitQuiesce();
    CHECK_EQ(rt.liveCells(), 0u);
}

// A second quiesce request issued after the worker has captured serveThrough for
// the first must not be completed by that (already-started) cut.
void quiesce_ticket_does_not_complete_from_old_cut() {
    Runtime rt(256, Runtime::Mode::Async);
    auto m = rt.makeMutator();
    CHECK(m != nullptr);

    Gate captured;
    Gate secondRound;
    std::atomic<int> captures{0};
    Runtime::TestHooks hooks;
    hooks.afterQuiesceCapture = [&] {
        const int n = captures.fetch_add(1) + 1;
        if (n == 1) captured.signalAndWait();
        if (n == 2) secondRound.signalAndWait();
    };
    rt.setTestHooks(std::move(hooks));

    std::atomic<bool> q1Done{false};
    std::atomic<bool> q2Done{false};
    std::thread q1([&] { rt.awaitQuiesce(); q1Done.store(true); });
    captured.waitReached();
    std::thread q2([&] { rt.awaitQuiesce(); q2Done.store(true); });
    captured.release();
    secondRound.waitReached();

    while (!q1Done.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(!q2Done.load(std::memory_order_acquire));
    secondRound.release();
    q1.join();
    q2.join();
    CHECK(q2Done.load(std::memory_order_acquire));
}

// Shutdown from a different thread must wake an idle worker and join it.  The
// stop_ flag is atomic and the worker re-checks it under the condition mutex, so
// the shutdown wake is never lost.
void shutdown_wakes_idle_worker() {
    auto rt = std::make_unique<Runtime>(256, Runtime::Mode::Async);
    std::atomic<bool> stopSet{false};
    std::atomic<bool> workerExited{false};
    Runtime::TestHooks hooks;
    hooks.shutdownSetStop = [&] { stopSet.store(true, std::memory_order_release); };
    hooks.workerExited = [&] { workerExited.store(true, std::memory_order_release); };
    rt->setTestHooks(std::move(hooks));

    auto m = rt->makeMutator();
    CHECK(m != nullptr);
    ObjHandle h = m->alloc();
    CHECK(h != kNullHandle);
    CHECK(m->rootRemove(h));
    rt->awaitQuiesce();
    m->close();
    m.reset();

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        rt.reset();
        destroyed.store(true, std::memory_order_release);
    });
    destroyer.join();
    CHECK(destroyed.load(std::memory_order_acquire));
    CHECK(stopSet.load(std::memory_order_acquire));
    CHECK(workerExited.load(std::memory_order_acquire));
}

// Two concurrent awaitQuiesce callers both complete (their tickets are distinct
// and each is served by a cut that covers it).
void repeated_quiesce_requests_complete() {
    Runtime rt(256, Runtime::Mode::Async);
    auto m = rt.makeMutator();
    CHECK(m != nullptr);
    ObjHandle h = m->alloc();
    CHECK(h != kNullHandle);
    CHECK(m->rootRemove(h));
    m->flush();          // publish the staged block before other threads quiesce

    std::atomic<bool> firstDone{false};
    std::atomic<bool> secondDone{false};
    std::thread q1([&] { rt.awaitQuiesce(); firstDone.store(true); });
    std::thread q2([&] { rt.awaitQuiesce(); secondDone.store(true); });
    q1.join();
    q2.join();
    CHECK(firstDone.load());
    CHECK(secondDone.load());
    CHECK_EQ(rt.liveCells(), 0u);
}

} // namespace

int main() {
    std::printf("[test_concurrency]\n");
    owner_capacity_does_not_alias();
    consistent_cut_rejects_cross_queue_transfer();
    publication_window_drains_without_reclaim();
    owner_exit_hands_off_survivor();
    quiesce_ticket_does_not_complete_from_old_cut();
    shutdown_wakes_idle_worker();
    repeated_quiesce_requests_complete();
    return aarc_test::failures() ? 1 : 0;
}
