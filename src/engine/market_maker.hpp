#pragma once
// ─── Market Making Support ────────────────────────────────────────────────────
// Provides quoting engine infrastructure for market makers:
// - Two-sided quote management (bid + ask within spread bounds)
// - Auto-cancel on fill to maintain delta neutral
// - Inventory skew adjustment
// - Quote refresh on book change

#include "../core/order.hpp"
#include "../book/order_book.hpp"
#include <optional>
#include <functional>

namespace obk {

struct QuoteParams {
    SymbolId symbol_id;
    ClientId client_id;
    Quantity qty;           // per side
    Price    bid_offset;    // offset from mid (typically > 0)
    Price    ask_offset;    // offset from mid (typically > 0)
    double   max_skew;      // maximum inventory skew adjustment factor
    int32_t  max_position;  // max net position (in lots, scaled)
};

struct Quote {
    OrderId  bid_order_id;
    OrderId  ask_order_id;
    Price    bid_price;
    Price    ask_price;
    Quantity bid_qty;
    Quantity ask_qty;
    Timestamp ts_quoted;
};

class MarketMaker {
public:
    using SubmitFn = std::function<OrderId(
        SymbolId, Side, OrderType, Price, Quantity, ClientId, TimeInForce, bool, bool)>;
    using CancelFn = std::function<bool(SymbolId, OrderId, ClientId)>;

    MarketMaker(QuoteParams params, SubmitFn submit, CancelFn cancel)
        : params_(std::move(params))
        , submit_(std::move(submit))
        , cancel_(std::move(cancel))
        , net_position_(0) {}

    // Refresh quotes based on new mid price
    void refresh(Price mid_price) {
        // Cancel stale quotes
        if (current_quote_) {
            cancel_(params_.symbol_id, current_quote_->bid_order_id, params_.client_id);
            cancel_(params_.symbol_id, current_quote_->ask_order_id, params_.client_id);
        }

        // Inventory skew: pull prices away from side with too much exposure
        Price skew = static_cast<Price>(net_position_ * params_.max_skew);

        Price bid = mid_price - params_.bid_offset - skew;
        Price ask = mid_price + params_.ask_offset - skew;

        // Quantity adjustment for inventory
        Quantity bid_qty = (net_position_ > 0)
            ? static_cast<Quantity>(params_.qty * 0.5) : params_.qty;
        Quantity ask_qty = (net_position_ < 0)
            ? static_cast<Quantity>(params_.qty * 0.5) : params_.qty;

        Quote q{};
        q.bid_price = bid;
        q.ask_price = ask;
        q.bid_qty   = bid_qty;
        q.ask_qty   = ask_qty;
        q.ts_quoted = now_ns();

        q.bid_order_id = submit_(params_.symbol_id, Side::Buy,  OrderType::Limit,
                                 bid, bid_qty, params_.client_id,
                                 TimeInForce::GTC, true/*post_only*/, false);
        q.ask_order_id = submit_(params_.symbol_id, Side::Sell, OrderType::Limit,
                                 ask, ask_qty, params_.client_id,
                                 TimeInForce::GTC, true/*post_only*/, false);
        current_quote_ = q;
    }

    // Called when our quote is filled
    void on_fill(Side side, Quantity qty) noexcept {
        if (side == Side::Buy)  net_position_ += static_cast<int32_t>(qty / QTY_SCALE);
        else                    net_position_ -= static_cast<int32_t>(qty / QTY_SCALE);
    }

    const std::optional<Quote>& current_quote() const noexcept { return current_quote_; }
    int32_t net_position() const noexcept { return net_position_; }

private:
    QuoteParams            params_;
    SubmitFn               submit_;
    CancelFn               cancel_;
    std::optional<Quote>   current_quote_;
    int32_t                net_position_;
};

} // namespace obk
