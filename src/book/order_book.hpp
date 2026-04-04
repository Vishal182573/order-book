#pragma once
// ─── Order Book (single symbol) ---------------------------------------
// Maintains bid and ask sides as sorted maps of PriceLevelQueues.
// Bids: descending (best bid = highest price = rbegin of map)
// Asks: ascending  (best ask = lowest price  = begin of map)
// All operations targeted to be O(log N) price levels, O(1) fills.

#include "price_level.hpp"
#include "../core/event_bus.hpp"
#include <map>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>

namespace obk {

// ─── Match Result ----------------------------------------------------------
// Yeh struct batata hai ki jab koi order book mein aata hai, toh kya-kya hua.
// Ismein yeh details hoti hain:
// matched: kya koi trade hua? (true/false)
// filled_qty: kitna quantity match hua?
// avg_price: average price kya thi jis par trade hua?
// trades: actual trades ki list (kya-kya trades hue)
struct MatchResult {
    bool      matched = false;
    Quantity  filled_qty = 0;
    Price     avg_price  = 0;
    std::vector<Trade> trades;
};


class OrderBook {
public:
    explicit OrderBook(SymbolId symbol_id, Symbol sym)
        : symbol_id_(symbol_id), symbol_(sym) {}

    // ── Order add ----------------------------------------------------------
    [[nodiscard]] MatchResult add_limit_order(Order* order) noexcept;
    [[nodiscard]] MatchResult add_market_order(Order* order) noexcept;
    [[nodiscard]] MatchResult add_stop_order(Order* order) noexcept;

    // ── Cancel / Modify ---------------------------------------------------   
    bool cancel_order(OrderId id) noexcept;
    bool modify_order(OrderId id, Price new_price, Quantity new_qty) noexcept;

    // ── Stop order triggering ---------------------------------------------
    // Returns triggered orders when last trade price crosses their trigger
    std::vector<Order*> check_stop_triggers(Price last_trade_price) noexcept;

    // ── Market data -------------------------------------------------------
    [[nodiscard]] std::optional<Price>    best_bid()    const noexcept;
    [[nodiscard]] std::optional<Price>    best_ask()    const noexcept;
    [[nodiscard]] std::optional<Price>    spread()      const noexcept;
    [[nodiscard]] double                  mid_price()   const noexcept;
    [[nodiscard]] TopOfBook               top_of_book() const noexcept;
    [[nodiscard]] OrderBookSnapshot       snapshot(size_t depth = 20) const noexcept;

    // ── Analytics ─────────────────────────────────────────────────────────
    [[nodiscard]] double order_imbalance()  const noexcept; // [-1, +1]
    [[nodiscard]] bool   has_liquidity_gap(Price bid, Price ask) const noexcept;

    [[nodiscard]] SymbolId symbol_id() const noexcept { return symbol_id_; }
    [[nodiscard]] size_t   bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t   ask_levels() const noexcept { return asks_.size(); }
    [[nodiscard]] size_t   order_count() const noexcept { return order_map_.size(); }

private:
    // ── Internal helpers ──────────────────────────────────────────────────
    MatchResult match_against_book(Order* taker) noexcept;
    Trade       create_trade(const Order* maker, Order* taker, Quantity qty) noexcept;
    void        insert_limit(Order* order) noexcept;
    void        remove_empty_levels() noexcept;
    PriceLevelQueue& get_or_create_level(Side side, Price price) noexcept;
    void        remove_level_if_empty(Side side, Price price) noexcept;

    // ── Data ──────────────────────────────────────────────────────────────
    SymbolId symbol_id_;
    Symbol   symbol_;

    // Sorted price levels
    // bids_: map key = price, highest price = best bid → use rbegin
    std::map<Price, PriceLevelQueue> bids_;
    // asks_: map key = price, lowest price = best ask  → use begin
    std::map<Price, PriceLevelQueue> asks_;

    // Fast order lookup by ID
    struct OrderEntry {
        Order* order;
        Side   side;
        Price  price;
    };
    std::unordered_map<OrderId, OrderEntry> order_map_;

    // Stop orders pending trigger (sorted by trigger price)
    std::map<Price, std::vector<Order*>> buy_stops_;   // trigger when price rises above
    std::map<Price, std::vector<Order*>> sell_stops_;  // trigger when price falls below

    // Trade ID counter
    TradeId next_trade_id_ = 1;
    Price   last_trade_price_ = 0;
};

// ──────────────────────────────────────────────────────────────────────────────
// IMPLEMENTATION
// ──────────────────────────────────────────────────────────────────────────────
inline MatchResult OrderBook::add_limit_order(Order* order) noexcept {
    // --- post-only: reject if crossing spread --------------------------------
    if (order->post_only) {
        if (order->is_buy()) {
            auto best = best_ask();
            if (best && order->price >= *best) {
                order->status = OrderStatus::Rejected;
                return {};
            }
        } else {
            auto best = best_bid();
            if (best && order->price <= *best) {
                order->status = OrderStatus::Rejected;
                return {};
            }
        }
    }

    // --- try to match --------------------------------------------------------
    MatchResult result = match_against_book(order);

    // --- rest any unfilled quantity ------------------------------------------
    if (order->is_active() && order->remaining_qty > 0 &&
        order->tif != TimeInForce::IOC && order->tif != TimeInForce::FOK) {
        insert_limit(order);
    } else if (order->tif == TimeInForce::IOC || order->tif == TimeInForce::FOK) {
        if (order->remaining_qty > 0) order->status = OrderStatus::Cancelled;
    }

    return result;
}

inline MatchResult OrderBook::add_market_order(Order* order) noexcept {
    order->price = (order->is_buy()) ? std::numeric_limits<Price>::max()
                                     : std::numeric_limits<Price>::min();
    return match_against_book(order);
}

inline MatchResult OrderBook::add_stop_order(Order* order) noexcept {
    // Place in stop trigger map; will be activated when price crosses trigger
    if (order->type == OrderType::StopLoss || order->type == OrderType::TakeProfit) {
        if (order->is_buy()) {
            buy_stops_[order->trigger_price].push_back(order);
        } else {
            sell_stops_[order->trigger_price].push_back(order);
        }
        order->status = OrderStatus::New;
    }
    return {};
}

inline MatchResult OrderBook::match_against_book(Order* taker) noexcept {
    MatchResult result;
    auto& opposite_side = taker->is_buy() ? asks_ : bids_;

    auto try_match = [&](auto levels_begin, auto levels_end) {
        std::vector<Price> to_erase;
        for (auto lvl_it = levels_begin; lvl_it != levels_end && taker->remaining_qty > 0; ++lvl_it) {
            auto& [lvl_price, level] = *lvl_it;

            // Price check
            bool crosses = taker->is_buy() ? (taker->price >= lvl_price)
                                           : (taker->price <= lvl_price);
            if (!crosses) break;

            while (!level.empty() && taker->remaining_qty > 0) {
                Order* maker = level.front();

                // Skip hidden orders in matching (they're still valid)
                Quantity fill_qty = std::min(taker->remaining_qty, maker->remaining_qty);
                if (fill_qty == 0) { level.pop_front(); continue; }

                Trade trade = create_trade(maker, taker, fill_qty);
                result.trades.push_back(trade);
                result.filled_qty += fill_qty;
                result.matched = true;
                last_trade_price_ = trade.price;

                // Update quantities
                taker->filled_qty  += fill_qty;
                taker->remaining_qty -= fill_qty;
                maker->filled_qty  += fill_qty;
                maker->remaining_qty -= fill_qty;
                level.reduce_front(fill_qty);

                // Maker fully filled?
                if (maker->remaining_qty == 0) {
                    maker->status = OrderStatus::Filled;
                    // Iceberg: replenish if there is still hidden reserve
                    if (maker->type == OrderType::Iceberg && maker->ice_remaining > 0) {
                        level.replenish_iceberg(maker);
                    } else {
                        level.pop_front();
                        order_map_.erase(maker->id);
                    }
                } else {
                    maker->status = OrderStatus::PartialFill;
                }
            }

            if (level.empty()) {
                to_erase.push_back(lvl_price);
            }
        }
        for (Price p : to_erase) {
            opposite_side.erase(p);
        }
    };

    if (taker->is_buy()) {
        try_match(opposite_side.begin(), opposite_side.end());
    } else {
        try_match(opposite_side.rbegin(), opposite_side.rend());
    }

    // Update taker status
    if (taker->remaining_qty == 0) {
        taker->status = OrderStatus::Filled;
    } else if (taker->filled_qty > 0) {
        taker->status = OrderStatus::PartialFill;
        result.avg_price = (result.filled_qty > 0)
            ? (result.avg_price * (result.filled_qty - result.filled_qty) + result.filled_qty * last_trade_price_) / result.filled_qty
            : 0;
    }

    return result;
}

inline Trade OrderBook::create_trade(const Order* maker, Order* taker, Quantity qty) noexcept {
    Trade t{};
    t.id             = next_trade_id_++;
    t.symbol_id      = symbol_id_;
    t.maker_order_id = maker->id;
    t.taker_order_id = taker->id;
    t.maker_client_id= maker->client_id;
    t.taker_client_id= taker->client_id;
    t.price          = maker->price;  // maker price rules
    t.qty            = qty;
    t.aggressor_side = taker->side;
    t.ts             = now_ns();
    t.notional       = (t.price * qty) / QTY_SCALE;
    return t;
}

inline void OrderBook::insert_limit(Order* order) noexcept {
    auto& levels = order->is_buy() ? bids_ : asks_;
    auto  it     = levels.find(order->price);
    if (it == levels.end()) {
        auto [ins_it, ok] = levels.emplace(order->price, PriceLevelQueue{order->price});
        it = ins_it;
    }
    it->second.push_back(order);
    order_map_[order->id] = {order, order->side, order->price};
}

inline bool OrderBook::cancel_order(OrderId id) noexcept {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return false;
    auto& [order, side, price] = it->second;
    auto& levels = (side == Side::Buy) ? bids_ : asks_;
    auto  lvl_it = levels.find(price);
    if (lvl_it != levels.end()) {
        lvl_it->second.remove(order);
        if (lvl_it->second.empty()) levels.erase(lvl_it);
    }
    order->status = OrderStatus::Cancelled;
    order_map_.erase(it);
    return true;
}

inline bool OrderBook::modify_order(OrderId id, Price new_price, Quantity new_qty) noexcept {
    auto it = order_map_.find(id);
    if (it == order_map_.end()) return false;
    auto& [order, side, price] = it->second;

    // Cancel and re-insert (loses queue priority if price changes)
    cancel_order(id);
    order->price        = new_price;
    order->qty          = new_qty;
    order->remaining_qty= new_qty - order->filled_qty;
    order->status       = OrderStatus::Modified;
    insert_limit(order);
    return true;
}

inline std::vector<Order*> OrderBook::check_stop_triggers(Price last_price) noexcept {
    std::vector<Order*> triggered;
    // Buy stops: trigger when price rises ABOVE trigger price
    for (auto it = buy_stops_.begin(); it != buy_stops_.end() && it->first <= last_price; ) {
        for (auto* o : it->second) { o->triggered = true; triggered.push_back(o); }
        it = buy_stops_.erase(it);
    }
    // Sell stops: trigger when price falls BELOW trigger price
    for (auto it = sell_stops_.upper_bound(last_price); it != sell_stops_.end(); ) {
        for (auto* o : it->second) { o->triggered = true; triggered.push_back(o); }
        it = sell_stops_.erase(it);
    }
    return triggered;
}

// ── Market data ───────────────────────────────────────────────────────────────
inline std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.rbegin()->first;
}
inline std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}
inline std::optional<Price> OrderBook::spread() const noexcept {
    auto bid = best_bid(); auto ask = best_ask();
    if (!bid || !ask) return std::nullopt;
    return *ask - *bid;
}
inline double OrderBook::mid_price() const noexcept {
    auto bid = best_bid(); auto ask = best_ask();
    if (!bid || !ask) return 0.0;
    return static_cast<double>(*bid + *ask) / 2.0;
}
inline TopOfBook OrderBook::top_of_book() const noexcept {
    TopOfBook tob{};
    tob.symbol_id = symbol_id_;
    tob.ts = now_ns();
    if (!bids_.empty()) {
        const auto& [p, lvl] = *bids_.rbegin();
        tob.bid_price = p; tob.bid_qty = lvl.visible_qty();
    }
    if (!asks_.empty()) {
        const auto& [p, lvl] = *asks_.begin();
        tob.ask_price = p; tob.ask_qty = lvl.visible_qty();
    }
    if (tob.bid_price && tob.ask_price) {
        tob.spread    = tob.ask_price - tob.bid_price;
        tob.mid_price = static_cast<double>(tob.bid_price + tob.ask_price) / 2.0;
    }
    return tob;
}

inline OrderBookSnapshot OrderBook::snapshot(size_t depth) const noexcept {
    OrderBookSnapshot snap;
    snap.symbol_id = symbol_id_;
    snap.ts        = now_ns();
    size_t n = 0;
    for (auto it = bids_.rbegin(); it != bids_.rend() && n < depth; ++it, ++n)
        snap.bids.push_back({it->first, it->second.visible_qty(), it->second.order_count()});
    n = 0;
    for (auto it = asks_.begin(); it != asks_.end() && n < depth; ++it, ++n)
        snap.asks.push_back({it->first, it->second.visible_qty(), it->second.order_count()});
    if (!snap.bids.empty() && !snap.asks.empty())
        snap.spread = snap.asks[0].price - snap.bids[0].price;
    snap.imbalance = order_imbalance();
    return snap;
}

inline double OrderBook::order_imbalance() const noexcept {
    Quantity bid_qty = 0, ask_qty = 0;
    size_t n = 0;
    for (auto it = bids_.rbegin(); it != bids_.rend() && n < 5; ++it, ++n) bid_qty += it->second.visible_qty();
    n = 0;
    for (auto it = asks_.begin();  it != asks_.end()  && n < 5; ++it, ++n) ask_qty += it->second.visible_qty();
    if (bid_qty + ask_qty == 0) return 0.0;
    return static_cast<double>(bid_qty - ask_qty) / static_cast<double>(bid_qty + ask_qty);
}

inline bool OrderBook::has_liquidity_gap(Price bid, Price ask) const noexcept {
    // A gap exists if there are no limit orders between bid and ask beyond N ticks
    Price gap = ask - bid;
    return gap > symbol_.tick_size * 10; // threshold: 10 ticks
}

} // namespace obk
