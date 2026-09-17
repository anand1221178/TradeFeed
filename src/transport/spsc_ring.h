#pragma once

#include <atomic>
#include <cstddef>
#include <cassert>

template <typename T, size_t Capacity>
class SPSCRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;

    // Each atomic on its own cache line to prevent false sharing
    // between producer (head) and consumer (tail).
    struct alignas(64) PaddedIndex {
        std::atomic<size_t> val{0};
    };

    PaddedIndex head_;
    PaddedIndex tail_;

    // Producer caches consumer tail, consumer caches producer head.
    // Avoids cross-core atomic load on every push/pop — only reload
    // when the cached value says the ring is full/empty.
    alignas(64) size_t cached_tail_ = 0;
    alignas(64) size_t cached_head_ = 0;

    alignas(64) T buffer_[Capacity];

public:
    bool push(const T& item) {
        const size_t head = head_.val.load(std::memory_order_relaxed);

        if (head - cached_tail_ >= Capacity) {
            cached_tail_ = tail_.val.load(std::memory_order_acquire);
            if (head - cached_tail_ >= Capacity)
                return false;
        }

        buffer_[head & MASK] = item;
        head_.val.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t tail = tail_.val.load(std::memory_order_relaxed);

        if (cached_head_ <= tail) {
            cached_head_ = head_.val.load(std::memory_order_acquire);
            if (cached_head_ <= tail)
                return false;
        }

        item = buffer_[tail & MASK];
        tail_.val.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return head_.val.load(std::memory_order_acquire) -
               tail_.val.load(std::memory_order_acquire);
    }

    bool empty() const { return size() == 0; }
    static constexpr size_t capacity() { return Capacity; }
};
