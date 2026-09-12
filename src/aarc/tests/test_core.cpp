// Phase 0 tests: cell pool, object representation, SPSC queue, GC messages.
//
// Covers (Todo_AddGC.md 9): allocate/free, reuse, full capacity, iteration,
// double-free protection, generation reuse safety; SPSC FIFO, wrap-around,
// queue full, concurrent producer/consumer stress (exactly 1 producer + 1
// consumer).

#include "core/cell.hpp"
#include "core/spsc_queue.hpp"
#include "core/gc_message.hpp"
#include "th_test.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace loxis::aarc;

// ---- object representation: the 9-byte payload really is 9 bytes ---------
namespace {
void test_payload_layout() {
    // If this were a natural struct {u8,u32,u32} alignment would inflate it
    // to 12 bytes; the pool must store a raw 9-byte unit instead.
    CHECK_EQ(kCellBytes, 9u);

    CellPool pool(16);
    ObjHandle h = pool.alloc();
    CHECK(h != kNullHandle);

    pool.setTag(h, 0xAB);
    pool.setId(h, 0x11223344u);
    pool.setData(h, 0x55667788u);
    CHECK_EQ(pool.getTag(h), 0xAB);
    CHECK_EQ(pool.getId(h), 0x11223344u);
    CHECK_EQ(pool.getData(h), 0x55667788u);

    // untouched bytes are zero (fresh allocation clears the payload)
    pool.free(h);
    ObjHandle h2 = pool.alloc();
    CHECK_EQ(pool.getTag(h2), 0u);
    CHECK_EQ(pool.getId(h2), 0u);
    CHECK_EQ(pool.getData(h2), 0u);
    pool.free(h2);
}
} // namespace

// ---- allocate / free / reuse / generation --------------------------------
namespace {
void test_alloc_free_reuse() {
    CellPool pool(8);
    CHECK_EQ(pool.used(), 0u);

    std::vector<ObjHandle> got;
    for (int i = 0; i < 8; ++i) {
        ObjHandle h = pool.alloc();
        CHECK(h != kNullHandle);
        got.push_back(h);
    }
    CHECK_EQ(pool.used(), 8u);

    // capacity exhausted -> null
    CHECK(pool.alloc() == kNullHandle);

    // double-free detection on the same handle
    CHECK(pool.free(got[0]));
    CHECK_EQ(pool.errorCount(), 0ull);
    CHECK(!pool.free(got[0]));        // second free -> error, not corruption
    CHECK_EQ(pool.errorCount(), 1ull);

    // stale-handle free (generation mismatch after a recycle) -> error, no-op
    ObjHandle h = pool.alloc();       // reuses got[0]'s slot with bumped gen
    CHECK(h.index == got[0].index);
    CHECK(h.gen != got[0].gen);       // generation must differ after recycle
    CHECK(!pool.free(got[0]));        // old handle is stale now
    CHECK_EQ(pool.errorCount(), 2ull);
    CHECK(pool.valid(h));
    CHECK(!pool.valid(got[0]));

    pool.free(h);
    CHECK_EQ(pool.used(), 7u);
}

void test_iterate() {
    CellPool pool(16);
    std::vector<uint32_t> idxs;
    for (int i = 0; i < 5; ++i)
        idxs.push_back(pool.alloc().index);
    std::vector<uint32_t> seen;
    pool.forEachLive([&](ObjHandle h) { seen.push_back(h.index); });
    CHECK_EQ(seen.size(), idxs.size());
    bool all = true;
    for (uint32_t i : idxs) {
        bool found = false;
        for (uint32_t s : seen)
            if (s == i) found = true;
        all = all && found;
    }
    CHECK(all);
}
} // namespace

// ---- SPSC ------------------------------------------------------------------
namespace {
using IntQ = SpscQueue<int, 8>;

void test_spsc_fifo_wrap() {
    IntQ q;
    CHECK(q.empty());

    // fill to capacity
    for (int i = 0; i < 8; ++i)
        CHECK(q.tryPush(i));
    CHECK(q.tryPush(99) == false);   // full
    CHECK_EQ(q.size(), 8u);

    // drain in FIFO order
    for (int i = 0; i < 8; ++i) {
        int v = -1;
        CHECK(q.tryPop(v));
        CHECK_EQ(v, i);
    }
    int v;
    CHECK(q.tryPop(v) == false);     // empty
    CHECK(q.empty());

    // wrap-around: push/pop far past one ring rotation
    for (int round = 0; round < 1000; ++round) {
        CHECK(q.tryPush(round));
        int out;
        CHECK(q.tryPop(out));
        CHECK_EQ(out, round);
    }
}

void test_spsc_concurrent_stress() {
    // Exactly ONE producer thread and ONE consumer thread.
    constexpr size_t kCap = 16;
    SpscQueue<int, kCap> q;
    constexpr int kTotal = 200000;

    std::atomic<bool> prodDone{false};
    std::atomic<long long> sum{0};

    std::thread producer([&] {
        for (int i = 0; i < kTotal; ++i) {
            while (!q.tryPush(i)) {
                std::this_thread::yield();   // never drop a message
            }
        }
        prodDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int received = 0;
        long long s = 0;
        while (received < kTotal) {
            int v;
            if (q.tryPop(v)) {
                s += v;
                ++received;
            } else if (!prodDone.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // if producer is done and queue empty, loop re-checks and exits
        }
        sum.store(s, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    CHECK_EQ(sum.load(), (long long)kTotal * (kTotal - 1) / 2);
}

void test_spsc_snapshot_drain() {
    // Worker-style: snapshot the producer tail, then drain only up to it.
    IntQ q;
    for (int i = 0; i < 5; ++i)
        q.tryPush(i);
    size_t snap = q.tailSnapshot();          // consumer may read producer tail
    CHECK_EQ(snap, 5u);
    for (int i = 0; i < 5; ++i) {
        int v;
        CHECK(q.tryPop(v));
        CHECK_EQ(v, i);
    }
    CHECK_EQ(q.headPosition(), 5u);
    int extra = 123;
    CHECK(q.tryPop(extra) == false);         // nothing beyond snapshot
    CHECK_EQ(extra, 123);
}
} // namespace

// ---- messages: handles survive a round-trip through the queue -------------
namespace {
void test_message_roundtrip() {
    SpscQueue<GCMessage, 4> q;
    ObjHandle src{7u, 42u};
    ObjHandle dst{3u, 9u};
    q.tryPush(GCMessage{GCOp::EdgeAdd, src, dst});
    GCMessage m;
    CHECK(q.tryPop(m));
    CHECK(m.op == GCOp::EdgeAdd);
    CHECK(m.a == src);
    CHECK(m.b == dst);
}
} // namespace

int main() {
    std::printf("[test_core]\n");
    test_payload_layout();
    test_alloc_free_reuse();
    test_iterate();
    test_spsc_fifo_wrap();
    test_spsc_snapshot_drain();
    test_spsc_concurrent_stress();
    test_message_roundtrip();
    return aarc_test::failures() ? 1 : 0;
}
