// ─── Order Book Engine — Main Demo ────────────────────────────────────────────
// Demonstrates the full pipeline:
//   Strategy signals → Order Generator → Matching Engine
//   → Risk / Portfolio / Execution Engines
// like this is the replication of the orders from the exchange only and not the full exchange. 
// once the strategy is ready after evaluating the results we will move the order to this order book it checks the slipage and all other things which will happen in real world if we make the order in the exchange. after that we send the trade to the execution engine using the trade engine. we will send the trade to the execution engine using the event bus.
/* Market Data Collector
        ↓
Redis Streams
        ↓
Time Series Warehouse
        ↓
Feature Generation Engine
        ↓
Strategy Engine
        ↓
Risk Engine
        ↓
Order Manager
        ↓
Order Book (C++)
        ↓
Trade Engine
        ↓
Portfolio Engine
        ↓
Execution Engine
        ↓
Exchange */

#include "core/types.hpp"
#include "core/event_bus.hpp"
#include "engine/matching_engine.hpp"
#include "engine/twap_vwap.hpp"
#include "engine/smart_order_router.hpp"
#include "engine/market_maker.hpp"
#include "downstream/risk_engine.hpp"
#include "downstream/portfolio_engine.hpp"
#include "downstream/execution_engine.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <random>

using namespace obk;
using namespace std::chrono_literals;

// ─── Simulated Strategy Signal ───────────────────────────────────────────────
enum class Signal { Buy, Sell, Flat };

// ─── Simulated Order Generator ───────────────────────────────────────────────
class OrderGenerator {
public:
    OrderGenerator(MatchingEngine& engine, SymbolId sym)
        : engine_(engine), sym_(sym), rng_(42) {}

    void on_signal(Signal sig, Price mid_price) {
        if (sig == Signal::Buy) {
            Price  limit = mid_price - to_price(1.0);  // bid 1 tick below mid
            engine_.submit_order(sym_, Side::Buy, OrderType::Limit,
                                 limit, to_quantity(0.1));
        } else if (sig == Signal::Sell) {
            Price  limit = mid_price + to_price(1.0);
            engine_.submit_order(sym_, Side::Sell, OrderType::Limit,
                                 limit, to_quantity(0.1));
        }
    }

    // Submit a market order
    void market_buy(double qty) {
        engine_.submit_order(sym_, Side::Buy, OrderType::Market,
                             0, to_quantity(qty));
    }

private:
    MatchingEngine& engine_;
    SymbolId        sym_;
    std::mt19937    rng_;
};

int main() {
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║   High-Performance C++ Order Book Engine v1.0   ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n\n";

    // ── 1. Event Bus ────────────────────────────────────────────────────────
    EventBus event_bus;

    // ── 2. Matching Engine ──────────────────────────────────────────────────
    MatchingEngine engine(event_bus);

    // ── 3. Register Symbol: BTC-USDT ─────────────────────────────────────────
    Symbol btc_usdt{};
    btc_usdt.id              = 1;
    btc_usdt.tick_size       = to_price(0.01);
    btc_usdt.lot_size        = to_quantity(0.00001);
    btc_usdt.price_precision = 2;
    btc_usdt.qty_precision   = 5;
    std::strcpy(btc_usdt.base,  "BTC");
    std::strcpy(btc_usdt.quote, "USDT");
    std::strcpy(btc_usdt.name,  "BTC-USDT");
    engine.add_symbol(btc_usdt);

    Symbol eth_usdt{};
    eth_usdt.id              = 2;
    eth_usdt.tick_size       = to_price(0.01);
    eth_usdt.lot_size        = to_quantity(0.0001);
    eth_usdt.price_precision = 2;
    eth_usdt.qty_precision   = 4;
    std::strcpy(eth_usdt.base,  "ETH");
    std::strcpy(eth_usdt.quote, "USDT");
    std::strcpy(eth_usdt.name,  "ETH-USDT");
    engine.add_symbol(eth_usdt);

    // ── 4. Subscribe Downstream Engines ─────────────────────────────────────
    RiskLimits limits{
        .max_order_qty      = to_quantity(100),
        .max_order_notional = to_price(10'000'000),
        .max_net_position   = to_quantity(50),
        .max_daily_notional = to_price(100'000'000),
        .max_loss           = to_price(50'000),
    };
    auto risk_engine  = std::make_shared<RiskEngine>(limits);
    auto portfolio    = std::make_shared<PortfolioEngine>();
    auto exec_engine  = std::make_shared<ExecutionEngine>();

    engine.subscribe(risk_engine);
    engine.subscribe(portfolio);
    engine.subscribe(exec_engine);

    // ── 5. Seed the BTC-USDT book with maker orders ─────────────────────────
    std::cout << "=== Seeding BTC-USDT order book ===\n";
    constexpr SymbolId BTC = 1;
    constexpr SymbolId ETH = 2;

    // Bids
    for (int i = 0; i < 5; ++i) {
        Price px = to_price(83000.0 - i * 10.0);
        engine.submit_order(BTC, Side::Buy, OrderType::Limit, px, to_quantity(0.5 + i * 0.1), 100);
    }
    // Asks
    for (int i = 0; i < 5; ++i) {
        Price px = to_price(83010.0 + i * 10.0);
        engine.submit_order(BTC, Side::Sell, OrderType::Limit, px, to_quantity(0.5 + i * 0.1), 101);
    }
    // Iceberg order
    Quantity ice_total   = to_quantity(5.0);
    Quantity ice_display = to_quantity(0.5);
    engine.submit_order(BTC, Side::Buy, OrderType::Iceberg,
                        to_price(82990.0), ice_total, 102,
                        TimeInForce::GTC, false, false, 0, ice_display);

    // Post-only ask
    engine.submit_order(BTC, Side::Sell, OrderType::Limit,
                        to_price(83050.0), to_quantity(1.0), 103,
                        TimeInForce::GTC, true /*post_only*/);

    // Stop loss
    engine.submit_order(BTC, Side::Sell, OrderType::StopLoss,
                        to_price(82000.0), to_quantity(1.0), 104,
                        TimeInForce::GTC, false, false,
                        to_price(82500.0) /*trigger*/);

    // Give matching threads time to process
    std::this_thread::sleep_for(50ms);

    // ── 6. Market order — triggers matching ─────────────────────────────────
    std::cout << "\n=== Market order: buy 0.3 BTC ===\n";
    engine.submit_order(BTC, Side::Buy, OrderType::Market, 0, to_quantity(0.3), 200);
    std::this_thread::sleep_for(50ms);

    // ── 7. Cancel an order ────────────────────────────────────────────────
    std::cout << "\n=== Cancel order 3 ===\n";
    engine.cancel_order(BTC, 3, 100);
    std::this_thread::sleep_for(10ms);

    // ── 8. Modify an order ────────────────────────────────────────────────
    std::cout << "\n=== Modify order 4 price to 83005 ===\n";
    engine.modify_order(BTC, 4, to_price(83005.0), to_quantity(0.5));
    std::this_thread::sleep_for(10ms);

    // ── 9. Strategy generator demo ───────────────────────────────────────
    std::cout << "\n=== Strategy Engine signals ===\n";
    OrderGenerator generator(engine, BTC);
    generator.on_signal(Signal::Buy,  to_price(83005.0));
    generator.on_signal(Signal::Sell, to_price(83005.0));
    std::this_thread::sleep_for(10ms);

    // ── 10. Board summary ─────────────────────────────────────────────────
    std::cout << "\n=== Top of Book ===\n";
    const auto* btc_book = engine.get_book(BTC);
    if (btc_book) {
        auto tob  = btc_book->top_of_book();
        auto snap = btc_book->snapshot(5);
        std::cout << "BTC-USDT\n";
        std::cout << "  Best Bid : " << from_price(tob.bid_price)
                  << " x " << from_qty(tob.bid_qty) << "\n";
        std::cout << "  Best Ask : " << from_price(tob.ask_price)
                  << " x " << from_qty(tob.ask_qty) << "\n";
        std::cout << "  Spread   : " << from_price(tob.spread) << " USDT\n";
        std::cout << "  Mid      : " << tob.mid_price << " USDT\n";
        std::cout << "  Imbalance: " << btc_book->order_imbalance() << "\n";
        std::cout << "  Bid levels: " << btc_book->bid_levels() << "\n";
        std::cout << "  Ask levels: " << btc_book->ask_levels() << "\n";
        std::cout << "  Open orders: " << btc_book->order_count() << "\n";
    }

    // ── 11. Slippage estimation ─────────────────────────────────────────
    std::cout << "\n=== Slippage Estimate: buy 2.0 BTC ===\n";
    if (btc_book) {
        auto snap = btc_book->snapshot(10);
        auto est  = SlippageEstimator::estimate(snap, Side::Buy,
                    to_quantity(2.0), to_price(83005.0));
        std::cout << "  Avg Price     : " << from_price(est.expected_avg_price) << "\n";
        std::cout << "  Worst Price   : " << from_price(est.worst_price) << "\n";
        std::cout << "  Fillable Qty  : " << from_qty(est.fillable_qty) << "\n";
        std::cout << "  Slippage (bps): " << est.slippage_bps << "\n";
        std::cout << "  Fully fillable: " << (est.fully_fillable ? "yes" : "no") << "\n";
    }

    // ── 12. SOR demo ──────────────────────────────────────────────────
    std::cout << "\n=== Smart Order Router demo ===\n";
    if (btc_book) {
        const auto* eth_book = engine.get_book(ETH);
        // Seed ETH book quickly for SOR demo
        engine.submit_order(ETH, Side::Sell, OrderType::Limit,
                            to_price(2100.0), to_quantity(10.0), 300);
        std::this_thread::sleep_for(20ms);
        if (eth_book) {
            auto btc_snap = btc_book->snapshot(10);
            auto eth_snap = eth_book->snapshot(10);
            std::vector<std::pair<Venue, OrderBookSnapshot>> venues = {
                {Venue{BTC, 0.9, to_price(10.0)}, btc_snap},
                {Venue{ETH, 0.6, to_price(0.5)}, eth_snap},
            };
            auto plan = SmartOrderRouter::route(venues, Side::Buy,
                                                to_quantity(1.0), to_price(83005.0));
            std::cout << "  Routing plan (" << plan.legs.size() << " legs):\n";
            for (auto& leg : plan.legs) {
                std::cout << "    Symbol=" << leg.symbol_id
                          << " Qty=" << from_qty(leg.qty)
                          << " ExpSlip=" << leg.expected_slippage_bps << " bps\n";
            }
            std::cout << "  Blended slippage: "
                      << plan.total_expected_slippage_bps << " bps\n";
        }
    }

    // ── 13. Pool stats ────────────────────────────────────────────────
    std::cout << "\n=== Object Pool Stats ===\n";
    std::cout << "  Orders allocated: " << engine.order_pool_usage() << " / 1048576\n";

    std::cout << "\n✓ Engine demo complete. Shutting down...\n";
    engine.stop();
    return 0;
}
