#pragma once
// ─── Risk Engine ──────────────────────────────────────────────────────────────
// Subscribes to trade events and enforces:
//   - Per-client position limits
//   - Max order size
//   - Max daily notional
//   - Loss limits (PnL drawdown)

#include "../core/event_bus.hpp"
#include <unordered_map>
#include <atomic>
#include <iostream>

namespace obk {

struct RiskLimits {
    Quantity max_order_qty;       // max single order size
    Price    max_order_notional;  // max single order value
    Quantity max_net_position;    // max net long or short
    Price    max_daily_notional;  // max total volume per day
    Price    max_loss;            // max drawdown (negative PnL)
};

struct ClientRiskState {
    Quantity net_position;
    Price    daily_notional;
    Price    realized_pnl;
    Price    unrealized_pnl;
};

class RiskEngine : public IEventHandler {
public:
    RiskEngine(RiskLimits global_limits)
        : global_limits_(std::move(global_limits)) {}

    // Pre-trade check (called before order submission)
    [[nodiscard]] bool check_order(const Order& order) const noexcept {
        if (order.qty > global_limits_.max_order_qty) {
            log("REJECT: order quantity exceeds limit");
            return false;
        }
        Price notional = (order.price * order.qty) / QTY_SCALE;
        if (notional > global_limits_.max_order_notional) {
            log("REJECT: order notional exceeds limit");
            return false;
        }
        auto it = client_state_.find(order.client_id);
        if (it != client_state_.end()) {
            if (it->second.daily_notional + notional > global_limits_.max_daily_notional) {
                log("REJECT: daily notional limit breached");
                return false;
            }
        }
        return true;
    }

    void on_event(const Event& ev) noexcept override {
        if (auto* te = std::get_if<TradeEvent>(&ev)) {
            on_trade(te->trade);
        }
    }

private:
    void on_trade(const Trade& t) noexcept {
        // Update maker
        auto& maker = client_state_[t.maker_client_id];
        if (t.aggressor_side == Side::Sell) {
            maker.net_position += t.qty; // maker was buyer
        } else {
            maker.net_position -= t.qty;
        }
        maker.daily_notional += t.notional;

        // Update taker
        auto& taker = client_state_[t.taker_client_id];
        if (t.aggressor_side == Side::Buy) {
            taker.net_position += t.qty;
        } else {
            taker.net_position -= t.qty;
        }
        taker.daily_notional += t.notional;

        // Check position limits
        for (auto& [cid, state] : client_state_) {
            if (std::abs(state.net_position) > global_limits_.max_net_position) {
                log("RISK BREACH: client " + std::to_string(cid) + " exceeded net position limit");
            }
            if (state.realized_pnl < -global_limits_.max_loss) {
                log("RISK BREACH: client " + std::to_string(cid) + " exceeded max loss limit");
            }
        }
    }

    static void log(const std::string& msg) noexcept {
        std::cerr << "[RiskEngine] " << msg << "\n";
    }

    RiskLimits global_limits_;
    mutable std::unordered_map<ClientId, ClientRiskState> client_state_;
};

} // namespace obk
