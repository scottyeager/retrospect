#pragma once

#include <atomic>
#include <array>
#include <cstddef>

namespace retrospect {

/// Lock-free single-producer single-consumer queue.
/// Fixed capacity, no dynamic allocation.
template <typename T, size_t Capacity>
class SpscQueue {
    static_assert(Capacity > 0, "Capacity must be > 0");

public:
    /// Push an item (producer/TUI thread only).
    /// Returns false if the queue is full.
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % (Capacity + 1);
        if (next == tail_.load(std::memory_order_acquire))
            return false; // full
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Pop an item (consumer/audio thread only).
    /// Returns false if the queue is empty.
    bool pop(T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false; // empty
        item = buf_[tail];
        tail_.store((tail + 1) % (Capacity + 1), std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity + 1> buf_{};  // one extra slot for full/empty distinction
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

} // namespace retrospect
