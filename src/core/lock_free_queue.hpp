#pragma once
// ─── Lock-Free SPSC Ring Buffer ───────────────────────────────────────────────
// Single-Producer Single-Consumer wait-free queue.
// Used for inter-thread communication on the hot path.
// Capacity MUST be a power of 2.

#include <atomic>
#include <array>
#include <optional>
#include <cassert>
#include <cstdint>
#include <new>  // std::hardware_destructive_interference_size

namespace obk {

#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
    static constexpr size_t CACHE_LINE = 64;
#endif

template<typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
public:
    SPSCQueue() : head_(0), tail_(0) {}

    // Producer side — returns false if full
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire)) return false; // full
        buf_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side — returns nullopt if empty
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return std::nullopt; // empty
        T item = buf_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        return (head_.load(std::memory_order_acquire) -
                tail_.load(std::memory_order_acquire)) & MASK;
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t MASK = Capacity - 1;

    alignas(CACHE_LINE) std::atomic<size_t> head_;
    alignas(CACHE_LINE) std::atomic<size_t> tail_;
    alignas(CACHE_LINE) std::array<T, Capacity> buf_;
};

// ─── Lock-Free MPSC Queue ────────────────────────────────────────────────────
// Multi-Producer Single-Consumer — useful when multiple strategy threads
// funnel orders into a single matching engine thread.
template<typename T, size_t Capacity>
class MPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

    struct Slot {
        alignas(CACHE_LINE) std::atomic<size_t> seq;
        T data;
    };
public:
    MPSCQueue() {
        for (size_t i = 0; i < Capacity; ++i) buf_[i].seq.store(i, std::memory_order_relaxed);
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool try_push(const T& item) noexcept {
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = buf_[pos & MASK];
            size_t seq = slot.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false; // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        Slot& slot = buf_[pos & MASK];
        slot.data = item;
        slot.seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Slot& slot = buf_[pos & MASK];
        size_t seq = slot.seq.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        if (diff < 0) return std::nullopt;
        dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
        T item = slot.data;
        slot.seq.store(pos + MASK + 1, std::memory_order_release);
        return item;
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    alignas(CACHE_LINE) std::atomic<size_t> enqueue_pos_;
    alignas(CACHE_LINE) std::atomic<size_t> dequeue_pos_;
    alignas(CACHE_LINE) std::array<Slot, Capacity> buf_;
};

} // namespace obk
