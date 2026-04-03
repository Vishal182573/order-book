#pragma once
// ─── Matching Engine ──────────────────────────────────────────────────────────
// Multi-symbol, multi-threaded matching engine.
//
// Architecture:
//   - One MatchingEngine instance per system.
//   - One OrderBook per symbol (each runs in its own thread).
//   - Orders arrive via a lock-free MPSC input queue.
//   - Results fan-out via EventBus to all downstream consumers.
//
// Thread model:
//   - Caller threads: push orders to input_queue_ (wait-free).
//   - Matching threads: one per symbol, drain the per-symbol SPSC queue.
//   - No shared state between matching threads (each owns its OrderBook).

#include "../book/order_book.hpp"
#include "../core/event_bus.hpp"
#include "../core/object_pool.hpp"
#include "../core/lock_free_queue.hpp"
#include "twap_vwap.hpp"
#include "smart_order_router.hpp"
#include <thread>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>
#include <functional>
#include <iostream>

#ifdef _MSC_VER
#include <immintrin.h>
#endif

namespace obk {

// ─── Order Command (tagged union) ─────────────────────────────────────────────
enum class CommandType : uint8_t {
    NewOrder    = 0,
    CancelOrder = 1,
    ModifyOrder = 2,
    Shutdown    = 3,
};

struct alignas(8) OrderCommand {
    CommandType type;
    SymbolId    symbol_id;
    union {
        Order*        order;   // NewOrder
        CancelRequest cancel;  // CancelOrder
        ModifyRequest modify;  // ModifyOrder
    };
};
static_assert(std::is_trivially_copyable_v<OrderCommand>);

// ─── Per-Symbol Worker ────────────────────────────────────────────────────────
class SymbolWorker {
    static constexpr size_t QUEUE_CAPACITY = 65'536;
    using Queue = SPSCQueue<OrderCommand, QUEUE_CAPACITY>;
public:
    SymbolWorker(Symbol sym, EventBus& bus, OrderPool& pool)
        : book_(sym.id, sym)
        , event_bus_(bus)
        , pool_(pool)
        , running_(false)
    {}

    void start() {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this]{ run(); });
    }

    void stop() {
        OrderCommand cmd{};
        cmd.type = CommandType::Shutdown;
        while (!queue_.try_push(cmd)) std::this_thread::yield();
        if (thread_.joinable()) thread_.join();
    }

    // Called from external threads (producers)
    [[nodiscard]] bool submit(const OrderCommand& cmd) noexcept {
        return queue_.try_push(cmd);
    }

    const OrderBook& book() const noexcept { return book_; }

private:
    void run() noexcept {
        while (running_.load(std::memory_order_relaxed)) {
            auto cmd_opt = queue_.try_pop();
            if (!cmd_opt) {
                // Busy-spin with a pause hint for low-latency
#ifdef _MSC_VER
                _mm_pause();
#else
                __builtin_ia32_pause();
#endif
                continue;
            }
            process(*cmd_opt);
        }
    }

    void process(const OrderCommand& cmd) noexcept {
        switch (cmd.type) {
        case CommandType::Shutdown:
            running_.store(false, std::memory_order_relaxed);
            break;

        case CommandType::NewOrder: {
            Order* order = cmd.order;
            MatchResult result;

            switch (order->type) {
            case OrderType::Market:
                result = book_.add_market_order(order);
                break;
            case OrderType::StopLoss:
            case OrderType::TakeProfit:
                result = book_.add_stop_order(order);
                break;
            default:
                result = book_.add_limit_order(order);
                break;
            }

            // Publish execution reports
            emit_exec_reports(order, result);

            // Check stop triggers after every trade
            if (!result.trades.empty()) {
                Price last_price = result.trades.back().price;
                auto triggered   = book_.check_stop_triggers(last_price);
                for (Order* stop_order : triggered) {
                    // Re-submit as market order
                    stop_order->type  = OrderType::Market;
                    stop_order->price = stop_order->is_buy()
                        ? std::numeric_limits<Price>::max()
                        : std::numeric_limits<Price>::min();
                    auto r2 = book_.add_market_order(stop_order);
                    emit_exec_reports(stop_order, r2);
                }
            }

            // Publish top-of-book update
            event_bus_.publish(TopOfBookEvent{book_.top_of_book()});
            break;
        }

        case CommandType::CancelOrder: {
            bool ok = book_.cancel_order(cmd.cancel.order_id);
            (void)ok;
            break;
        }

        case CommandType::ModifyOrder: {
            const auto& m = cmd.modify;
            bool ok = book_.modify_order(m.order_id, m.new_price, m.new_qty);
            (void)ok;
            break;
        }
        }
    }

    void emit_exec_reports(Order* order, const MatchResult& result) noexcept {
        // Trade events
        for (const auto& trade : result.trades) {
            event_bus_.publish(TradeEvent{trade});
        }
        // Execution report
        ExecutionReport rpt{};
        rpt.order_id     = order->id;
        rpt.client_id    = order->client_id;
        rpt.symbol_id    = order->symbol_id;
        rpt.order_status = order->status;
        rpt.side         = order->side;
        rpt.price        = order->price;
        rpt.qty          = order->qty;
        rpt.leaves_qty   = order->remaining_qty;
        rpt.cum_qty      = order->filled_qty;
        rpt.ts           = now_ns();
        if (!result.trades.empty()) {
            rpt.last_qty   = result.trades.back().qty;
            rpt.last_price = result.trades.back().price;
        }
        event_bus_.publish(rpt);
    }

    OrderBook        book_;
    Queue            queue_;
    EventBus&        event_bus_;
    OrderPool&       pool_;
    std::atomic<bool> running_;
    std::thread      thread_;
};

// ─── Matching Engine ──────────────────────────────────────────────────────────
class MatchingEngine {
public:
    explicit MatchingEngine(EventBus& bus)
        : event_bus_(bus), next_order_id_(1) {}

    ~MatchingEngine() { stop(); }

    // Register a new symbol
    void add_symbol(const Symbol& sym) {
        workers_.emplace(sym.id,
            std::make_unique<SymbolWorker>(sym, event_bus_, pool_));
        workers_[sym.id]->start();
    }

    // Subscribe to all events
    size_t subscribe(std::shared_ptr<IEventHandler> handler) {
        return event_bus_.subscribe(std::move(handler));
    }

    // Submit a new order (thread-safe, non-blocking)
    [[nodiscard]] OrderId submit_order(
        SymbolId  symbol_id,
        Side      side,
        OrderType type,
        Price     price,
        Quantity  qty,
        ClientId  client_id = 0,
        TimeInForce tif     = TimeInForce::GTC,
        bool      post_only = false,
        bool      hidden    = false,
        Price     trigger   = 0,
        Quantity  display   = 0    // iceberg display qty
    ) {
        auto* worker = get_worker(symbol_id);
        if (!worker) return 0;

        Order* order = pool_.acquire();
        if (!order) return 0; // pool exhausted

        *order = Order{};
        order->id           = next_order_id_.fetch_add(1, std::memory_order_relaxed);
        order->client_id    = client_id;
        order->symbol_id    = symbol_id;
        order->side         = side;
        order->type         = type;
        order->price        = price;
        order->qty          = qty;
        order->remaining_qty= qty;
        order->tif          = tif;
        order->post_only    = post_only;
        order->hidden       = hidden;
        order->trigger_price= trigger;
        order->display_qty  = display;
        order->ice_remaining= (type == OrderType::Iceberg) ? (qty - display) : 0;
        order->status       = OrderStatus::PendingNew;
        order->ts_created   = now_ns();

        OrderCommand cmd{};
        cmd.type      = CommandType::NewOrder;
        cmd.symbol_id = symbol_id;
        cmd.order     = order;

        while (!worker->submit(cmd)) std::this_thread::yield();
        return order->id;
    }

    // Cancel order
    bool cancel_order(SymbolId symbol_id, OrderId order_id, ClientId client_id = 0) {
        auto* worker = get_worker(symbol_id);
        if (!worker) return false;
        OrderCommand cmd{};
        cmd.type          = CommandType::CancelOrder;
        cmd.symbol_id     = symbol_id;
        cmd.cancel.order_id  = order_id;
        cmd.cancel.client_id = client_id;
        cmd.cancel.ts        = now_ns();
        while (!worker->submit(cmd)) std::this_thread::yield();
        return true;
    }

    // Modify order
    bool modify_order(SymbolId symbol_id, OrderId order_id,
                      Price new_price, Quantity new_qty) {
        auto* worker = get_worker(symbol_id);
        if (!worker) return false;
        OrderCommand cmd{};
        cmd.type           = CommandType::ModifyOrder;
        cmd.symbol_id      = symbol_id;
        cmd.modify.order_id  = order_id;
        cmd.modify.new_price = new_price;
        cmd.modify.new_qty   = new_qty;
        cmd.modify.ts        = now_ns();
        while (!worker->submit(cmd)) std::this_thread::yield();
        return true;
    }

    const OrderBook* get_book(SymbolId id) const {
        auto it = workers_.find(id);
        return (it != workers_.end()) ? &it->second->book() : nullptr;
    }

    void stop() {
        for (auto& [id, worker] : workers_) worker->stop();
        workers_.clear();
    }

    size_t order_pool_usage() const noexcept { return pool_.allocated_count(); }

private:
    SymbolWorker* get_worker(SymbolId id) {
        auto it = workers_.find(id);
        return (it != workers_.end()) ? it->second.get() : nullptr;
    }

    EventBus&    event_bus_;
    OrderPool    pool_;
    std::unordered_map<SymbolId, std::unique_ptr<SymbolWorker>> workers_;
    std::atomic<OrderId> next_order_id_;
};

} // namespace obk
