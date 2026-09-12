// AARC core: single-producer / single-consumer lock-free ring buffer.
//
// Ordering contract (see Todo_AddGC.md 0.3):
//   * Exactly ONE producer thread calls tryPush / capacity available.
//   * Exactly ONE consumer thread calls tryPop / drainUntil.
//   * No mutex is used.  Push/pop use release/acquire on the tail/head
//     indexes so that a value written before tail.release is visible after
//     head acquires tail (Vyukov-style SPSC).
//   * Overflow is reported to the producer (return false); the producer must
//     not silently drop the message (retry / slow path is the caller's job).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace loxis { namespace aarc {

template <class T, size_t Capacity>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "SpscQueue capacity must be a power of two");
    static_assert(Capacity >= 2, "SpscQueue capacity too small");

public:
    static constexpr size_t kCapacity = Capacity;

    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // ---- producer side -------------------------------------------------

    // Returns false when the ring is full.  Nothing is dropped by us, but the
    // caller must retry (spin / slow path) to preserve the message.
    bool tryPush(const T& v) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= kCapacity)
            return false;
        slot_[tail & (kCapacity - 1)] = v;          // write payload
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ---- producer span API (zero-copy batch publish) --------------------
    // Reserve up to `max` CONTIGUOUS slots for the producer to write in place.
    // Returns the number reserved (0 = no room).  The producer writes the run
    // and publishes it with ONE commitSpan() release store, instead of one
    // store per message.  Uncommitted slots are invisible to the consumer, so
    // the cut protocol is unchanged: the tail store is still the publication
    // point, it just moves in larger steps.
    size_t beginSpan(size_t max) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        size_t room = kCapacity - (tail - head);
        const size_t contig = kCapacity - (tail & (kCapacity - 1));
        if (contig < room) room = contig;
        return room < max ? room : max;
    }

    // First slot of the reserved span (valid until the next beginSpan).
    T* spanPtr() noexcept {
        return &slot_[tail_.load(std::memory_order_relaxed) & (kCapacity - 1)];
    }

    // Publish the first `n` slots of the reserved span.
    void commitSpan(size_t n) noexcept {
        tail_.store(tail_.load(std::memory_order_relaxed) + n,
                    std::memory_order_release);
    }

    // ---- consumer side -------------------------------------------------

    // Returns false when the ring is empty (nothing currently readable).
    bool tryPop(T& out) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail)
            return false;
        out = slot_[head & (kCapacity - 1)];        // read payload
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // A consumer-side snapshot of the producer's write position.  Draining
    // until head_ == tailSnapshot() covers exactly the messages pushed before
    // the snapshot; anything pushed afterwards is left for a later round.
    size_t tailSnapshot() const noexcept {
        return tail_.load(std::memory_order_acquire);
    }

    // Head position, for the consumer to know how far it has drained.
    size_t headPosition() const noexcept {
        return head_.load(std::memory_order_relaxed);
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_acquire);
    }

    // Approximate number of buffered messages.  Safe to call from the consumer
    // while the producer runs.
    size_t size() const noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        return t >= h ? t - h : 0;
    }

private:
    static constexpr size_t kPad = 64;
    alignas(kPad) std::atomic<size_t> head_{0};     // consumer
    alignas(kPad) std::atomic<size_t> tail_{0};     // producer
    alignas(kPad) T slot_[Capacity];
};

}} // namespace loxis::aarc
