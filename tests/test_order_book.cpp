// ─── Unit Tests: Order Book Engine ────────────────────────────────────────────
#include <gtest/gtest.h>
#include "../src/core/types.hpp"
#include "../src/core/lock_free_queue.hpp"
#include "../src/core/object_pool.hpp"
#include "../src/book/price_level.hpp"
#include "../src/book/order_book.hpp"
#include "../src/engine/matching_engine.hpp"
#include "../src/book/slippage_estimator.hpp"

using namespace obk;

// ─── Helpers ─────────────────────────────────────────────────────────────────
static Symbol make_symbol(SymbolId id = 1) {
    Symbol s{};
    s.id         = id;
    s.tick_size  = to_price(0.01);
    s.lot_size   = to_quantity(0.0001);
    std::strcpy(s.name, "TEST-USDT");
    return s;
}

static Order make_order(OrderId id, Side side, Price price, Quantity qty,
                         OrderType type = OrderType::Limit) {
    Order o{};
    o.id           = id;
    o.symbol_id    = 1;
    o.side         = side;
    o.type         = type;
    o.price        = price;
    o.qty          = qty;
    o.remaining_qty= qty;
    o.tif          = TimeInForce::GTC;
    o.status       = OrderStatus::New;
    o.ts_created   = now_ns();
    return o;
}

// ─── Types Tests ──────────────────────────────────────────────────────────────
TEST(TypesTest, PriceConversion) {
    EXPECT_EQ(to_price(100.0),    100'000'000LL);
    EXPECT_EQ(to_price(0.01),         10'000LL);
    EXPECT_NEAR(from_price(to_price(83000.5)), 83000.5, 1e-6);
}

TEST(TypesTest, QuantityConversion) {
    EXPECT_EQ(to_quantity(1.5), 1'500'000LL);
    EXPECT_NEAR(from_qty(to_quantity(0.12345)), 0.12345, 1e-5);
}

// ─── Lock-free Queue Tests ───────────────────────────────────────────────────
TEST(SPSCQueueTest, PushPop) {
    SPSCQueue<int, 8> q;
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.try_push(42));
    EXPECT_FALSE(q.empty());
    auto v = q.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueTest, Full) {
    SPSCQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4)); // 4th slot is sentinel
}

TEST(SPSCQueueTest, FIFO) {
    SPSCQueue<int, 16> q;
    for (int i = 0; i < 10; ++i) q.try_push(i);
    for (int i = 0; i < 10; ++i) {
        auto v = q.try_pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
}

TEST(MPSCQueueTest, MultiProducer) {
    MPSCQueue<int, 64> q;
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&, t]{
            for (int i = 0; i < 10; ++i) {
                while (!q.try_push(t * 100 + i)) std::this_thread::yield();
            }
        });
    }
    for (auto& th : producers) th.join();

    int count = 0;
    while (q.try_pop()) ++count;
    EXPECT_EQ(count, 40);
}

// ─── Object Pool Tests ────────────────────────────────────────────────────────
TEST(ObjectPoolTest, AcquireRelease) {
    ObjectPool<int, 8> pool;
    EXPECT_EQ(pool.free_count(), 8u);
    int* a = pool.acquire();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(pool.free_count(), 7u);
    pool.release(a);
    EXPECT_EQ(pool.free_count(), 8u);
}

TEST(ObjectPoolTest, Exhaustion) {
    ObjectPool<int, 4> pool;
    int* p1 = pool.acquire();
    int* p2 = pool.acquire();
    int* p3 = pool.acquire();
    int* p4 = pool.acquire();
    EXPECT_EQ(pool.acquire(), nullptr);  // exhausted
    pool.release(p1); pool.release(p2); pool.release(p3); pool.release(p4);
}

// ─── Price Level Tests ────────────────────────────────────────────────────────
TEST(PriceLevelTest, PushFront) {
    PriceLevelQueue lvl(to_price(100.0));
    Order o1 = make_order(1, Side::Buy, to_price(100.0), to_quantity(1.0));
    Order o2 = make_order(2, Side::Buy, to_price(100.0), to_quantity(2.0));
    lvl.push_back(&o1);
    lvl.push_back(&o2);
    EXPECT_EQ(lvl.order_count(), 2u);
    EXPECT_EQ(lvl.front()->id, 1u);  // FIFO — o1 first
}

TEST(PriceLevelTest, Remove) {
    PriceLevelQueue lvl(to_price(100.0));
    Order o1 = make_order(1, Side::Buy, to_price(100.0), to_quantity(1.0));
    Order o2 = make_order(2, Side::Buy, to_price(100.0), to_quantity(1.0));
    lvl.push_back(&o1);
    lvl.push_back(&o2);
    lvl.remove(&o1);
    EXPECT_EQ(lvl.order_count(), 1u);
    EXPECT_EQ(lvl.front()->id, 2u);
}

// ─── Order Book Tests ─────────────────────────────────────────────────────────
class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        book = std::make_unique<OrderBook>(1, make_symbol());
    }
    std::unique_ptr<OrderBook> book;
    OrderId next_id = 1;

    Order* make_alloc(Side side, double price, double qty,
                       OrderType type = OrderType::Limit) {
        static Order storage[256];
        static size_t idx = 0;
        storage[idx] = make_order(next_id++, side, to_price(price), to_quantity(qty), type);
        return &storage[idx++];
    }
};

TEST_F(OrderBookTest, BestBidAsk) {
    book->add_limit_order(make_alloc(Side::Buy,  99.0, 1.0));
    book->add_limit_order(make_alloc(Side::Sell, 101.0, 1.0));
    EXPECT_EQ(book->best_bid(), to_price(99.0));
    EXPECT_EQ(book->best_ask(), to_price(101.0));
    EXPECT_EQ(book->spread(),   to_price(2.0));
}

TEST_F(OrderBookTest, BasicMatch) {
    book->add_limit_order(make_alloc(Side::Sell, 100.0, 1.0)); // maker
    auto taker = make_alloc(Side::Buy, 100.0, 1.0);
    auto result = book->add_limit_order(taker);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.filled_qty, to_quantity(1.0));
    EXPECT_EQ(result.trades.size(), 1u);
    EXPECT_EQ(taker->status, OrderStatus::Filled);
}

TEST_F(OrderBookTest, PartialFill) {
    book->add_limit_order(make_alloc(Side::Sell, 100.0, 0.5)); // 0.5 available
    auto taker = make_alloc(Side::Buy, 100.0, 1.0);            // want 1.0
    auto result = book->add_limit_order(taker);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.filled_qty, to_quantity(0.5));
    EXPECT_EQ(taker->status, OrderStatus::PartialFill);
    EXPECT_EQ(taker->remaining_qty, to_quantity(0.5));
}

TEST_F(OrderBookTest, MarketOrderFullFill) {
    book->add_limit_order(make_alloc(Side::Sell, 100.0, 2.0));
    auto taker  = make_alloc(Side::Buy, 0.0, 1.0, OrderType::Market);
    auto result = book->add_market_order(taker);
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.filled_qty, to_quantity(1.0));
    EXPECT_EQ(taker->status, OrderStatus::Filled);
}

TEST_F(OrderBookTest, CancelOrder) {
    auto o = make_alloc(Side::Buy, 99.0, 1.0);
    book->add_limit_order(o);
    EXPECT_TRUE(book->cancel_order(o->id));
    EXPECT_EQ(book->bid_levels(), 0u);
    EXPECT_EQ(o->status, OrderStatus::Cancelled);
}

TEST_F(OrderBookTest, PostOnlyReject) {
    book->add_limit_order(make_alloc(Side::Sell, 100.0, 1.0)); // ask at 100
    auto po = make_alloc(Side::Buy, 101.0, 1.0); // crossing ask
    po->post_only = true;
    book->add_limit_order(po);
    EXPECT_EQ(po->status, OrderStatus::Rejected);
}

TEST_F(OrderBookTest, OrderImbalance) {
    for (int i = 0; i < 3; ++i)
        book->add_limit_order(make_alloc(Side::Buy,  99.0 - i, 1.0));
    book->add_limit_order(make_alloc(Side::Sell, 101.0, 0.1));
    double imb = book->order_imbalance();
    EXPECT_GT(imb, 0.0);  // more bids → positive imbalance
}

TEST_F(OrderBookTest, Snapshot) {
    book->add_limit_order(make_alloc(Side::Buy,  99.0, 1.0));
    book->add_limit_order(make_alloc(Side::Sell, 101.0, 1.0));
    auto snap = book->snapshot(5);
    EXPECT_EQ(snap.bids.size(), 1u);
    EXPECT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.bids[0].price, to_price(99.0));
    EXPECT_EQ(snap.asks[0].price, to_price(101.0));
}

// ─── Slippage Estimator Tests ─────────────────────────────────────────────────
TEST(SlippageTest, NoSlippageExactFit) {
    OrderBookSnapshot snap;
    snap.asks.push_back({to_price(100.0), to_quantity(1.0), 1});
    auto est = SlippageEstimator::estimate(snap, Side::Buy,
                                           to_quantity(1.0), to_price(100.0));
    EXPECT_TRUE(est.fully_fillable);
    EXPECT_NEAR(est.slippage_bps, 0.0, 1.0);
}

TEST(SlippageTest, MultiLevelSlippage) {
    OrderBookSnapshot snap;
    snap.asks.push_back({to_price(100.0), to_quantity(1.0), 1});
    snap.asks.push_back({to_price(101.0), to_quantity(1.0), 1});
    auto est = SlippageEstimator::estimate(snap, Side::Buy,
                                           to_quantity(2.0), to_price(100.0));
    EXPECT_TRUE(est.fully_fillable);
    EXPECT_GT(est.slippage_bps, 0.0); // we paid above ref
}

// ─── Matching Engine Integration Test ────────────────────────────────────────
TEST(EngineTest, EndToEnd) {
    EventBus bus;
    MatchingEngine engine(bus);
    engine.add_symbol(make_symbol(1));

    std::atomic<int> trade_count{0};
    struct Counter : IEventHandler {
        std::atomic<int>& cnt;
        Counter(std::atomic<int>& c) : cnt(c) {}
        void on_event(const Event& ev) noexcept override {
            if (std::get_if<TradeEvent>(&ev)) ++cnt;
        }
    };
    engine.subscribe(std::make_shared<Counter>(trade_count));

    // Seed sell side
    engine.submit_order(1, Side::Sell, OrderType::Limit, to_price(100.0),
                        to_quantity(1.0), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Buy market order — should match
    engine.submit_order(1, Side::Buy, OrderType::Market, 0,
                        to_quantity(1.0), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_GE(trade_count.load(), 1);
    engine.stop();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
