#pragma once
// ─── TWAP / VWAP Execution Algorithms ─────────────────────────────────────────
// TWAP: Time-Weighted Average Price — divides order into equal time slices.
// VWAP: Volume-Weighted Average Price — sizes slices proportional to historical volume.

#include "../core/order.hpp"
#include <vector>
#include <chrono>
#include <cmath>
#include <numeric>

namespace obk {

// ─── Historical volume profile (for VWAP) ─────────────────────────────────────
// In production, this is populated by the Time Series Engine (Candle data).
struct VolumeProfile {
    std::vector<double> volume_weights; // one per time bucket
    size_t num_buckets;
};

// ─── TWAP Executor ────────────────────────────────────────────────────────────
class TWAPExecutor {
public:
    struct Slice {
        Timestamp release_time;
        Quantity  qty;
        bool      released;
    };

    // Build slices from an order
    static std::vector<Slice> build(const Order& order) {
        std::vector<Slice> slices;
        if (order.num_slices <= 0) return slices;

        Quantity per_slice     = order.qty / order.num_slices;
        Quantity remainder     = order.qty % order.num_slices;
        int64_t  time_interval = (order.end_time - order.start_time) / order.num_slices;

        for (int32_t i = 0; i < order.num_slices; ++i) {
            Slice s{};
            s.release_time = order.start_time + i * time_interval;
            s.qty          = per_slice + (i == 0 ? remainder : 0);
            s.released     = false;
            slices.push_back(s);
        }
        return slices;
    }

    // Returns the next slice to release (if time has come), else nullopt
    static std::optional<Slice*> next_slice(std::vector<Slice>& slices) {
        Timestamp now = now_ns();
        for (auto& s : slices) {
            if (!s.released && now >= s.release_time) {
                s.released = true;
                return &s;
            }
        }
        return std::nullopt;
    }
};

// ─── VWAP Executor ────────────────────────────────────────────────────────────
class VWAPExecutor {
public:
    struct Slice {
        Timestamp release_time;
        Quantity  qty;
        double    target_weight;
        bool      released;
    };

    static std::vector<Slice> build(const Order& order, const VolumeProfile& profile) {
        std::vector<Slice> slices;
        size_t n           = profile.num_buckets;
        int64_t interval   = (order.end_time - order.start_time) / static_cast<int64_t>(n);

        // Normalise weights
        double total_weight = std::accumulate(profile.volume_weights.begin(),
                                              profile.volume_weights.end(), 0.0);
        if (total_weight == 0.0) total_weight = 1.0;

        Quantity allocated = 0;
        for (size_t i = 0; i < n; ++i) {
            double w   = profile.volume_weights[i] / total_weight;
            Quantity q = static_cast<Quantity>(std::round(from_qty(order.qty) * w * QTY_SCALE));
            Slice s{};
            s.release_time  = order.start_time + static_cast<int64_t>(i) * interval;
            s.qty           = q;
            s.target_weight = w;
            s.released      = false;
            slices.push_back(s);
            allocated += q;
        }
        // Adjust rounding error into last slice
        if (!slices.empty())
            slices.back().qty += (order.qty - allocated);
        return slices;
    }
};

// ─── Algo Scheduler ───────────────────────────────────────────────────────────
// Manages pending TWAP/VWAP orders and fires child orders at the right time.
// Call tick() from a periodic scheduler (e.g., every 100ms).
class AlgoScheduler {
    struct AlgoState {
        Order*              parent;
        std::vector<TWAPExecutor::Slice> twap_slices;
        std::vector<VWAPExecutor::Slice> vwap_slices;
        bool is_twap;
    };
public:
    using OrderCallback = std::function<void(SymbolId, Side, Quantity, Price)>;

    void submit(Order* order, const VolumeProfile& profile, OrderCallback cb) {
        AlgoState state;
        state.parent  = order;
        state.is_twap = (order->type == OrderType::TWAP);
        if (state.is_twap) {
            state.twap_slices = TWAPExecutor::build(*order);
        } else {
            state.vwap_slices = VWAPExecutor::build(*order, profile);
        }
        state_map_[order->id] = std::move(state);
        callbacks_[order->id] = std::move(cb);
    }

    // Called periodically
    void tick() {
        Timestamp now = now_ns();
        for (auto& [oid, state] : state_map_) {
            auto& cb = callbacks_[oid];
            Order* parent = state.parent;

            if (state.is_twap) {
                if (auto* slice = TWAPExecutor::next_slice(state.twap_slices).value_or(nullptr)) {
                    cb(parent->symbol_id, parent->side, slice->qty, 0 /* market */);
                }
            } else {
                for (auto& sl : state.vwap_slices) {
                    if (!sl.released && now >= sl.release_time) {
                        sl.released = true;
                        cb(parent->symbol_id, parent->side, sl.qty, 0 /* market */);
                        break;
                    }
                }
            }
        }
    }

private:
    std::unordered_map<OrderId, AlgoState>     state_map_;
    std::unordered_map<OrderId, OrderCallback> callbacks_;
};

} // namespace obk
