#pragma once
// ─── Slippage Estimator ───────────────────────────────────────────────────────
// Estimates expected slippage for a given market order on the current book.
// Used by Smart Order Router (SOR) and Risk Engine.

#include "order_book.hpp"

namespace obk {

struct SlippageEstimate {
    Price    expected_avg_price;
    Price    worst_price;
    Price    slippage_cost;       // (avg_price - ref_price) * qty
    double   slippage_bps;        // slippage in basis points
    Quantity fillable_qty;        // how much we can actually fill
    bool     fully_fillable;
};

class SlippageEstimator {
public:
    // Estimate slippage walking the simulated book
    [[nodiscard]] static SlippageEstimate estimate(
        const OrderBookSnapshot& snap,
        Side      side,
        Quantity  order_qty,
        Price     ref_price      // mid-price or last trade
    ) noexcept {
        SlippageEstimate est{};
        const auto& levels = (side == Side::Buy) ? snap.asks : snap.bids;

        Quantity remaining  = order_qty;
        Price    cost       = 0;
        Price    worst      = (side == Side::Buy) ? 0 : std::numeric_limits<Price>::max();

        for (const auto& lvl : levels) {
            if (remaining == 0) break;
            Quantity fill = std::min(remaining, lvl.qty);
            cost      += fill * lvl.price / QTY_SCALE;
            remaining -= fill;
            worst      = (side == Side::Buy) ? std::max(worst, lvl.price)
                                             : std::min(worst, lvl.price);
        }

        Quantity filled = order_qty - remaining;
        est.fillable_qty      = filled;
        est.fully_fillable    = (remaining == 0);
        est.worst_price       = worst;
        est.expected_avg_price= (filled > 0) ? (cost * QTY_SCALE / filled) : 0;
        est.slippage_cost     = (side == Side::Buy)
            ? (est.expected_avg_price - ref_price) * filled / QTY_SCALE
            : (ref_price - est.expected_avg_price) * filled / QTY_SCALE;
        est.slippage_bps      = (ref_price > 0)
            ? static_cast<double>(est.slippage_cost) / (ref_price * filled / QTY_SCALE) * 10000.0
            : 0.0;
        return est;
    }
};

} // namespace obk
