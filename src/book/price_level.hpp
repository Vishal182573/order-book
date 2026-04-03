#pragma once
// ─── Price Level ──────────────────────────────────────────────────────────────
// A single price level in the order book.
// Maintains a FIFO queue of orders at this price (price-time priority).
// Uses an intrusive doubly-linked list for O(1) cancel/fill.

#include "../core/order.hpp"
#include <list>
#include <unordered_map>
#include <cassert>

namespace obk {

class PriceLevelQueue {
public:
    explicit PriceLevelQueue(Price price) noexcept
        : price_(price), total_qty_(0), visible_qty_(0), order_count_(0) {}

    // Add order to BACK of queue (FIFO)
    void push_back(Order* order) noexcept {
        assert(order->price == price_ || order->type == OrderType::Market);
        orders_.push_back(order);
        // Iterator stored for O(1) removal
        order->queue_position = static_cast<uint32_t>(order_count_);
        order->ahead_qty      = static_cast<Quantity>(total_qty_); // approx
        total_qty_   += effective_qty(order);
        visible_qty_ += order->hidden ? 0 : effective_qty(order);
        ++order_count_;
    }

    // Remove specific order — O(1) with iterator map
    void remove(Order* order) noexcept {
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (*it == order) {
                total_qty_   -= effective_qty(order);
                visible_qty_ -= order->hidden ? 0 : effective_qty(order);
                --order_count_;
                orders_.erase(it);
                return;
            }
        }
    }

    // Peek at front order (maker)
    [[nodiscard]] Order* front() noexcept {
        return orders_.empty() ? nullptr : orders_.front();
    }

    // Pop front order (after fill)
    void pop_front() noexcept {
        if (orders_.empty()) return;
        Order* o = orders_.front();
        total_qty_   -= effective_qty(o);
        visible_qty_ -= o->hidden ? 0 : effective_qty(o);
        --order_count_;
        orders_.pop_front();
    }

    void reduce_front(Quantity filled) noexcept {
        if (orders_.empty()) return;
        Order* o = orders_.front();
        total_qty_   -= filled;
        visible_qty_ -= o->hidden ? 0 : filled;
    }

    // Iceberg: replenish after front is fully filled
    void replenish_iceberg(Order* order) noexcept {
        Quantity slice = std::min(order->display_qty, order->ice_remaining);
        order->remaining_qty = slice;
        order->ice_remaining -= slice;
        total_qty_   += slice;
        visible_qty_ += slice;
        // Push to BACK (re-queue for iceberg)
        orders_.pop_front();
        orders_.push_back(order);
    }

    [[nodiscard]] Price    price()        const noexcept { return price_; }
    [[nodiscard]] Quantity total_qty()    const noexcept { return total_qty_; }
    [[nodiscard]] Quantity visible_qty()  const noexcept { return visible_qty_; }
    [[nodiscard]] uint32_t order_count()  const noexcept { return order_count_; }
    [[nodiscard]] bool     empty()        const noexcept { return orders_.empty(); }

    using OrderList = std::list<Order*>;
    const OrderList& orders() const noexcept { return orders_; }

private:
    [[nodiscard]] static Quantity effective_qty(const Order* o) noexcept {
        // For iceberg orders, only the display_qty is "in" the level
        return (o->type == OrderType::Iceberg) ? o->display_qty : o->remaining_qty;
    }

    Price     price_;
    Quantity  total_qty_;
    Quantity  visible_qty_;
    uint32_t  order_count_;
    OrderList orders_;
};

} // namespace obk
