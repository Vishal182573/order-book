#pragma once
// ─── Event Bus  ───────────────────────────────────────────────────────────────
// Type-erased, lock-free event bus for distributing execution reports and
// market-data events to Risk Engine, Portfolio Engine, and Execution Engine.

#include "order.hpp"
#include "lock_free_queue.hpp"
#include <functional>
#include <variant>
#include <vector>
#include <memory>
#include <mutex>

namespace obk {

// ─── Event types ──────────────────────────────────────────────────────────────
struct OrderAccepted  { Order order; };
struct OrderRejected  { Order order; char reason[64]; };
struct OrderCancelled { Order order; };
struct OrderModified  { Order order; };
struct TradeEvent     { Trade trade; };
struct TopOfBookEvent { TopOfBook tob; };

using Event = std::variant<
    OrderAccepted,
    OrderRejected,
    OrderCancelled,
    OrderModified,
    TradeEvent,
    TopOfBookEvent,
    ExecutionReport
>;

// ─── Event Handler interface ──────────────────────────────────────────────────
class IEventHandler {
public:
    virtual ~IEventHandler() = default;
    virtual void on_event(const Event& event) noexcept = 0;
};

// ─── Event Bus ────────────────────────────────────────────────────────────────
// Multi-cast: one publisher, many subscribers.
// Each subscriber gets its own SPSC queue to avoid contention.
class EventBus {
    static constexpr size_t QUEUE_CAPACITY = 65'536;
    using Queue = SPSCQueue<Event, QUEUE_CAPACITY>;

public:
    // Subscribe — returns subscriber id
    size_t subscribe(std::shared_ptr<IEventHandler> handler) {
        std::lock_guard lk(mu_);
        auto q = std::make_shared<Queue>();
        size_t id = subs_.size();
        subs_.push_back({std::move(handler), std::move(q)});
        return id;
    }

    // Publish to all subscribers (called from matching engine thread)
    void publish(const Event& ev) noexcept {
        for (auto& [handler, queue] : subs_) {
            if (!queue->try_push(ev)) {
                // Queue overflow — log and continue (non-blocking)
                // In production, circuit-break or apply backpressure
            }
        }
    }

    // Drain queues — called by each subscriber's thread
    void drain(size_t sub_id) noexcept {
        if (sub_id >= subs_.size()) return;
        auto& [handler, queue] = subs_[sub_id];
        while (auto ev = queue->try_pop()) {
            handler->on_event(*ev);
        }
    }

    size_t subscriber_count() const noexcept { return subs_.size(); }

private:
    struct Subscriber {
        std::shared_ptr<IEventHandler> handler;
        std::shared_ptr<Queue>         queue;
    };
    std::mutex              mu_;
    std::vector<Subscriber> subs_;
};

} // namespace obk
