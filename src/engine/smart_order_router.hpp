#pragma once
// ─── Smart Order Router (SOR) ─────────────────────────────────────────────────
// Routes large orders across multiple symbols/venues to minimize slippage.
// In this system, "venues" are the internal multi-symbol order books.
// SOR can also support cross-asset matching (e.g., BTC-USDT via BTC-ETH + ETH-USDT).

#include "../book/slippage_estimator.hpp"
#include <algorithm>
#include <vector>

namespace obk {

struct Venue {
    SymbolId symbol_id;
    double   liquidity_score; // higher = more liquid
    Price    spread;
};

struct RoutingPlan {
    struct Leg {
        SymbolId symbol_id;
        Quantity qty;
        Price    expected_price;
        double   expected_slippage_bps;
    };
    std::vector<Leg> legs;
    Quantity total_qty;
    double   total_expected_slippage_bps;
};

class SmartOrderRouter {
public:
    // Rank venues by lowest expected slippage for the given order
    [[nodiscard]] static RoutingPlan route(
        const std::vector<std::pair<Venue, OrderBookSnapshot>>& venues,
        Side     side,
        Quantity order_qty,
        Price    ref_price
    ) {
        RoutingPlan plan{};
        plan.total_qty = order_qty;

        // Score each venue
        struct VenueScore {
            size_t idx;
            SlippageEstimate est;
        };
        std::vector<VenueScore> scores;
        for (size_t i = 0; i < venues.size(); ++i) {
            auto est = SlippageEstimator::estimate(venues[i].second, side, order_qty, ref_price);
            scores.push_back({i, est});
        }

        // Sort by slippage ascending
        std::sort(scores.begin(), scores.end(), [](const VenueScore& a, const VenueScore& b){
            return a.est.slippage_bps < b.est.slippage_bps;
        });

        // Allocate quantity greedily to best venues
        Quantity remaining = order_qty;
        for (auto& vs : scores) {
            if (remaining == 0) break;
            const auto& v   = venues[vs.idx].first;
            const auto& est = vs.est;
            Quantity alloc  = std::min(remaining, est.fillable_qty);
            if (alloc == 0) continue;

            RoutingPlan::Leg leg{};
            leg.symbol_id              = v.symbol_id;
            leg.qty                    = alloc;
            leg.expected_price         = est.expected_avg_price;
            leg.expected_slippage_bps  = est.slippage_bps;
            plan.legs.push_back(leg);
            remaining -= alloc;
        }

        // Compute blended slippage
        double total_notional = 0, weighted_slip = 0;
        for (auto& leg : plan.legs) {
            double notional = from_qty(leg.qty) * from_price(leg.expected_price);
            total_notional  += notional;
            weighted_slip   += leg.expected_slippage_bps * notional;
        }
        plan.total_expected_slippage_bps = (total_notional > 0)
            ? weighted_slip / total_notional : 0.0;

        return plan;
    }
};

} // namespace obk
