#pragma once
// ─── Object Pool  ─────────────────────────────────────────────────────────────
// Pre-allocated pool of Order objects to avoid heap allocations on the hot path.
// Thread-safe via atomic free-list head.

#include "order.hpp"
#include <vector>
#include <atomic>
#include <cassert>
#include <cstddef>

namespace obk {

template<typename T, size_t PoolSize>
class ObjectPool {
public:
    ObjectPool() : pool_(PoolSize), next_(PoolSize) {
        // Build the free list
        for (size_t i = 0; i < PoolSize - 1; ++i)
            next_[i] = static_cast<uint32_t>(i + 1);
        next_[PoolSize - 1] = INVALID;
        free_head_.store(0, std::memory_order_relaxed);
        allocated_.store(0, std::memory_order_relaxed);
    }

    // Acquire an object — returns pointer or nullptr if pool is exhausted
    [[nodiscard]] T* acquire() noexcept {
        uint32_t head = free_head_.load(std::memory_order_acquire);
        while (head != INVALID) {
            uint32_t next = next_[head];
            if (free_head_.compare_exchange_weak(head, next,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                allocated_.fetch_add(1, std::memory_order_relaxed);
                return &pool_[head];
            }
        }
        return nullptr; // pool exhausted
    }

    // Release an object back to the pool
    void release(T* obj) noexcept {
        assert(obj >= &pool_[0] && obj < &pool_[PoolSize]);
        uint32_t idx = static_cast<uint32_t>(obj - &pool_[0]);
        uint32_t head = free_head_.load(std::memory_order_relaxed);
        do { next_[idx] = head; }
        while (!free_head_.compare_exchange_weak(head, idx,
                std::memory_order_release,
                std::memory_order_relaxed));
        allocated_.fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t allocated_count() const noexcept {
        return allocated_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t free_count() const noexcept {
        return PoolSize - allocated_count();
    }
    static constexpr size_t capacity() noexcept { return PoolSize; }

private:
    static constexpr uint32_t INVALID = UINT32_MAX;

    alignas(64) std::vector<T>        pool_;
    alignas(64) std::vector<uint32_t> next_;
    alignas(64) std::atomic<uint32_t>          free_head_;
    alignas(64) std::atomic<size_t>            allocated_;
};

// Pre-sized pools for typical use cases
using OrderPool = ObjectPool<Order, 1'048'576>; // 1M orders

} // namespace obk
