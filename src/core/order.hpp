#pragma once
#include "types.hpp"
#include <atomic>
#include <string>

namespace obk {

// ─── Order ────────────────────────────────────────────────────────────────────
// Kept POD-like and cache-line friendly (64 bytes target for hot fields)
struct alignas(64) Order {
    // ── Identity (hot) ─────────────────────────────────────────────────────
    OrderId     id;
    ClientId    client_id;
    SymbolId    symbol_id;
    Price       price;          // limit price (0 for market)
    Quantity    qty;            // original quantity
    Quantity    remaining_qty;  // still open
    Quantity    filled_qty;
    Timestamp   ts_created;     // nanoseconds
    Timestamp   ts_updated;

    // ── Classification ────────────────────────────────────────────────────
    Side        side;
    OrderType   type;
    TimeInForce tif;
    OrderStatus status;

    // ── Features ──────────────────────────────────────────────────────────
    bool        post_only;      // reject if would immediately match
    bool        hidden;         // not visible in order book depth
    bool        reduce_only;    // can only reduce position size

    // ── Stop / TP trigger ────────────────────────────────────────────────
    Price       trigger_price;  // for stop/tp orders
    bool        triggered;

    // ── Iceberg ──────────────────────────────────────────────────────────
    Quantity    display_qty;    // visible quantity (iceberg)
    Quantity    ice_remaining;  // hidden reserve

    // ── TWAP/VWAP ────────────────────────────────────────────────────────
    Timestamp   start_time;
    Timestamp   end_time;
    int32_t     num_slices;

    // ── Queue position tracking ───────────────────────────────────────────
    uint32_t    queue_position; // estimated position at this price level
    Quantity    ahead_qty;      // estimated quantity ahead in queue

    // ── Padding to 128 bytes ──────────────────────────────────────────────
    uint8_t     _pad[4];

    // ── Helpers ──────────────────────────────────────────────────────────
    [[nodiscard]] bool is_active() const noexcept {
        return status == OrderStatus::New || status == OrderStatus::PartialFill;
    }
    [[nodiscard]] bool is_buy()  const noexcept { return side == Side::Buy; }
    [[nodiscard]] bool is_sell() const noexcept { return side == Side::Sell; }
    [[nodiscard]] bool is_fully_filled() const noexcept { return remaining_qty == 0; }
    [[nodiscard]] Price notional() const noexcept { return price * qty / QTY_SCALE; }
};

// ─── Trade ────────────────────────────────────────────────────────────────────
struct alignas(32) Trade {
    TradeId   id;
    SymbolId  symbol_id;
    OrderId   maker_order_id;   // passive (resting)
    OrderId   taker_order_id;   // aggressive (incoming)
    ClientId  maker_client_id;
    ClientId  taker_client_id;
    Price     price;
    Quantity  qty;
    Side      aggressor_side;   // side of the taker
    Timestamp ts;

    // Derived analytics
    Price     notional;         // price * qty / QTY_SCALE
    Price     maker_fee;
    Price     taker_fee;
};

// ─── Execution Report ─────────────────────────────────────────────────────────
struct ExecutionReport {
    OrderId       order_id;
    ClientId      client_id;
    SymbolId      symbol_id;
    TradeId       trade_id;       // 0 if not a trade
    ExecutionType exec_type;
    OrderStatus   order_status;
    Side          side;
    Price         price;
    Quantity      qty;
    Quantity      last_qty;       // this fill quantity
    Price         last_price;     // this fill price
    Quantity      leaves_qty;     // remaining
    Quantity      cum_qty;        // total filled so far
    Price         avg_price;      // weighted average fill price
    Timestamp     ts;
    char          reject_reason[64];
};

// ─── Market Data Structs ──────────────────────────────────────────────────────
struct PriceLevel {
    Price    price;
    Quantity qty;
    uint32_t order_count;
};

struct TopOfBook {
    SymbolId symbol_id;
    Price    bid_price;
    Quantity bid_qty;
    Price    ask_price;
    Quantity ask_qty;
    Price    spread;        // ask - bid
    double   mid_price;     // (ask + bid) / 2.0
    Timestamp ts;
};

struct OrderBookSnapshot {
    SymbolId                  symbol_id;
    Timestamp                 ts;
    std::vector<PriceLevel>   bids;    // sorted descending
    std::vector<PriceLevel>   asks;    // sorted ascending
    Price                     spread;
    double                    imbalance; // (bid_qty - ask_qty) / (bid_qty + ask_qty)
};

// ─── Order modification request ───────────────────────────────────────────────
struct ModifyRequest {
    OrderId  order_id;
    SymbolId symbol_id;
    Price    new_price;
    Quantity new_qty;
    Timestamp ts;
};

// ─── Cancel request ───────────────────────────────────────────────────────────
struct CancelRequest {
    OrderId  order_id;
    SymbolId symbol_id;
    ClientId client_id;
    Timestamp ts;
};

} // namespace obk
