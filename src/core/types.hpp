#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <chrono>
#include <array>

namespace obk {

// ─── Fundamental numeric types ──────────────────────────────────────────────
// Price stored as integer (price * PRICE_SCALE) to avoid floating point issues
using Price    = int64_t;   // e.g. 12345.67 → 1234567000 (6-decimal scale)
using Quantity = int64_t;   // e.g. 10.005 BTC → 10005000 (6-decimal scale)
using OrderId  = uint64_t;
using TradeId  = uint64_t;
using SymbolId = uint32_t;
using ClientId = uint32_t;

static constexpr int64_t PRICE_SCALE = 1000000LL;    // 6 decimal places will be used for price calculations
static constexpr int64_t QTY_SCALE   = 1000000LL;

// ------ Timestamp --------------------------------------------------------------
using Timestamp = int64_t;  // nanoseconds since epoch


inline Timestamp now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Breakdown:

// Part	Meaning
// system_clock::now()	current time
// time_since_epoch()	time since 1970
// duration_cast	convert to nanoseconds
// count()	get integer




// ------- Enumerations ---------------------------------------------------------------------
enum class Side : uint8_t { Buy = 0, Sell = 1 }; // uint8_t is used to save space as it takes 1 byte

enum class OrderType : uint8_t {
    Limit       = 0,
    Market      = 1,
    StopLoss    = 2,
    TakeProfit  = 3,
    Iceberg     = 4,
    TWAP        = 5,
    VWAP        = 6,
};

enum class TimeInForce : uint8_t {
    GTC  = 0,  // Good Till Cancel
    IOC  = 1,  // Immediate Or Cancel
    FOK  = 2,  // Fill Or Kill
    GTD  = 3,  // Good Till Date
    Day  = 4, // EOD
};

enum class OrderStatus : uint8_t {
    New          = 0,
    PartialFill  = 1,
    Filled       = 2,
    Cancelled    = 3,
    Rejected     = 4,
    Expired      = 5,
    PendingNew   = 6,
    PendingCancel= 7,
    Modified     = 8,
};

enum class ExecutionType : uint8_t {
    New       = 0,
    Partial   = 1,
    Fill      = 2,
    Cancelled = 3,
    Replaced  = 4,
    Rejected  = 5,
    Trade     = 6,
};

// ------- Symbol ----------------------------------------------------------------------
struct Symbol {
    SymbolId    id;
    char        base[16];    // e.g. "BTC"
    char        quote[16];   // e.g. "USDT"
    char        name[32];    // e.g. "BTC-USDT"
    Price       tick_size;   // minimum price increment
    Quantity    lot_size;    // minimum quantity increment
    Price       min_notional;// minimum order value
    int32_t     price_precision;
    int32_t     qty_precision;
};

// ------- Utility conversions ----------------------------------------------------------------------
inline Price    to_price(double p) noexcept    { return static_cast<Price>(p * PRICE_SCALE + 0.5); }
inline Quantity to_quantity(double q) noexcept { return static_cast<Quantity>(q * QTY_SCALE + 0.5); }
inline double   from_price(Price p) noexcept   { return static_cast<double>(p) / PRICE_SCALE; }
inline double   from_qty(Quantity q) noexcept  { return static_cast<double>(q) / QTY_SCALE; }

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

} // namespace obk
