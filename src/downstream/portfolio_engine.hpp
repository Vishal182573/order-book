#pragma once
// ─── Portfolio Engine ──────────────────────────────────────────────────────────
// Tracks fills → positions → PnL per client per symbol.

#include "../core/event_bus.hpp"
#include <unordered_map>
#include <string>
#include <iostream>

namespace obk {

struct Position {
    SymbolId symbol_id;
    ClientId client_id;
    Quantity net_qty;        // positive = long, negative = short
    Price    avg_cost;       // average entry price
    Price    realized_pnl;
    Price    unrealized_pnl; // needs mark-to-market
    Quantity total_bought;
    Quantity total_sold;
};

class PortfolioEngine : public IEventHandler {
public:
    void on_event(const Event& ev) noexcept override {
        if (auto* te = std::get_if<TradeEvent>(&ev)) {
            on_trade(te->trade);
        } else if (auto* tob = std::get_if<TopOfBookEvent>(&ev)) {
            mark_to_market(tob->tob);
        }
    }

    const Position* get_position(ClientId cid, SymbolId sid) const noexcept {
        auto it = positions_.find(make_key(cid, sid));
        return (it != positions_.end()) ? &it->second : nullptr;
    }

private:
    void on_trade(const Trade& t) noexcept {
        // Update maker position
        update_position(t.maker_client_id, t.symbol_id,
                        (t.aggressor_side == Side::Sell) ? t.qty : -t.qty,
                        t.price);
        // Update taker position
        update_position(t.taker_client_id, t.symbol_id,
                        (t.aggressor_side == Side::Buy) ? t.qty : -t.qty,
                        t.price);
    }

    void update_position(ClientId cid, SymbolId sid, Quantity qty_delta, Price fill_price) noexcept {
        auto& pos = positions_[make_key(cid, sid)];
        pos.symbol_id = sid;
        pos.client_id = cid;

        if (qty_delta > 0) {
            pos.total_bought += qty_delta;
            // Update avg cost
            Quantity new_net = pos.net_qty + qty_delta;
            if (new_net == 0) {
                pos.avg_cost = 0;
            } else if (pos.net_qty >= 0) {
                pos.avg_cost = (pos.avg_cost * pos.net_qty + fill_price * qty_delta) / new_net;
            } else {
                // Covering short: realize PnL
                Quantity cover = std::min(std::abs(pos.net_qty), qty_delta);
                pos.realized_pnl += (pos.avg_cost - fill_price) * cover / QTY_SCALE;
            }
        } else {
            pos.total_sold -= qty_delta;
            Quantity sell_qty = -qty_delta;
            if (pos.net_qty > 0) {
                Quantity close  = std::min(pos.net_qty, sell_qty);
                pos.realized_pnl += (fill_price - pos.avg_cost) * close / QTY_SCALE;
            }
        }
        pos.net_qty += qty_delta;
    }

    void mark_to_market(const TopOfBook& tob) noexcept {
        Price mid = static_cast<Price>(tob.mid_price);
        for (auto& [key, pos] : positions_) {
            if (pos.symbol_id == tob.symbol_id) {
                pos.unrealized_pnl = (mid - pos.avg_cost) * pos.net_qty / QTY_SCALE;
            }
        }
    }

    static uint64_t make_key(ClientId cid, SymbolId sid) noexcept {
        return (static_cast<uint64_t>(cid) << 32) | sid;
    }

    std::unordered_map<uint64_t, Position> positions_;
};

} // namespace obk
