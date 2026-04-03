#pragma once
// ─── Execution Engine ──────────────────────────────────────────────────────────
// Receives execution reports from the internal matching engine and
// forwards them to external exchanges (simulated here as a log/callback).

#include "../core/event_bus.hpp"
#include <iostream>
#include <functional>

namespace obk {

class ExecutionEngine : public IEventHandler {
public:
    using ExchangeCallback = std::function<void(const ExecutionReport&)>;

    explicit ExecutionEngine(ExchangeCallback cb = nullptr)
        : exchange_callback_(std::move(cb)) {}

    void on_event(const Event& ev) noexcept override {
        if (auto* rpt = std::get_if<ExecutionReport>(&ev)) {
            on_execution_report(*rpt);
        } else if (auto* te = std::get_if<TradeEvent>(&ev)) {
            on_trade(te->trade);
        }
    }

private:
    void on_execution_report(const ExecutionReport& rpt) noexcept {
        // In production: forward to FIX gateway / exchange REST/WS API
        if (exchange_callback_) exchange_callback_(rpt);
        std::cout << "[ExecEngine] Order=" << rpt.order_id
                  << " Status=" << static_cast<int>(rpt.order_status)
                  << " Filled=" << from_qty(rpt.cum_qty)
                  << " Leaves=" << from_qty(rpt.leaves_qty) << "\n";
    }

    void on_trade(const Trade& t) noexcept {
        std::cout << "[ExecEngine] Trade id=" << t.id
                  << " sym=" << t.symbol_id
                  << " price=" << from_price(t.price)
                  << " qty="   << from_qty(t.qty) << "\n";
    }

    ExchangeCallback exchange_callback_;
};

} // namespace obk
