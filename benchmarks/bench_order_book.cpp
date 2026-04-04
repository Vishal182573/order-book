// ─── Benchmarks: Order Book Engine -----------------------------------

// Software development (especially High-Frequency Trading ya Quant systems) mein benchmark ka matlab hota hai apne code ki speed, efficiency aur performance ko maapna (measure karna). Isme hum aisa code likhte hain jo hamare main system ke components ko hazaaron ya laakhon baar run karta hai aur yeh check karta hai ki execution mein kitne nanoseconds ya microseconds lag rahe hain. Kyunki trading mein speed hi sab kuch hai, benchmarks ensure karte hain ki aapke system mein koi slow parts (bottlenecks) na ban rahein.
// basically benchmark file is used to test the performance of the code like which specific portion of code leads to more time to execute
#include <benchmark/benchmark.h>
#include "../src/core/lock_free_queue.hpp"
#include "../src/core/object_pool.hpp"
#include "../src/book/order_book.hpp"
#include "../src/engine/matching_engine.hpp"

using namespace obk;

static Symbol make_sym() {
    Symbol s{}; s.id = 1; s.tick_size = to_price(0.01); s.lot_size = to_quantity(0.0001);
    std::strcpy(s.name, "BTC-USDT"); return s;
}

// ─── SPSC Queue throughput ----------------------------------------------------
// SPSC stands for Single Producer Single Consumer, it is a type of lock(mutex) free queue which is used to  push and pop the data from the queue to check how much speed we can achieve without locks
static void BM_SPSCQueue(benchmark::State& state) {
    SPSCQueue<int, 65536> q;
    for (auto _ : state) {
        q.try_push(42);
        benchmark::DoNotOptimize(q.try_pop());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SPSCQueue)->ThreadRange(1, 2);

// ─── Object Pool allocate/release -------------------------------------------
// Object pool is a technique used to reduce the overhead of memory allocation and deallocation 
// basically it is used to store the objects in a pool and reuse them when needed
// Yeh benchmark check karta hai ki pool se naya Order object maangne (acquire) aur use wapas free karne (release) mein kitna negligible time lagta hai.
static void BM_ObjectPool(benchmark::State& state) {
    ObjectPool<Order, 1024> pool;
    for (auto _ : state) {
        Order* o = pool.acquire();
        benchmark::DoNotOptimize(o);
        if (o) pool.release(o);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPool);

// ─── Limit order insert (no match) ---------------------------------------
// Yeh benchmark check karta hai ki jab koi limit order book mein insert hota hai aur usse koi match nahi milta (yani woh sirf book mein add hota hai), toh usmein kitna time lagta hai.
// how much time it takes to insert a limit order in the book when there is no match
static void BM_LimitInsert(benchmark::State& state) {
    OrderBook book(1, make_sym());
    static Order orders[65536];
    OrderId id = 1;
    for (auto _ : state) {
        size_t i = id % 65536;
        orders[i] = Order{};
        orders[i].id          = id++;
        orders[i].symbol_id   = 1;
        orders[i].side        = (id % 2) ? Side::Buy : Side::Sell;
        orders[i].type        = OrderType::Limit;
        orders[i].price       = to_price(100.0 - (id % 50) * 0.1);  // spread bids
        orders[i].qty         = to_quantity(1.0);
        orders[i].remaining_qty = to_quantity(1.0);
        orders[i].tif         = TimeInForce::GTC;
        orders[i].status      = OrderStatus::New;
        benchmark::DoNotOptimize(book.add_limit_order(&orders[i]));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LimitInsert);

// ─── Limit order match (crossing) ---------------------------------------------
// Jab ek Buy order aur Sell order exactly match (cross) karte hain, toh process mein condition check hoti hain, quantity minus hoti hai aur trade complete hota hai. BM_LimitMatch is matching condition ki throughput track karta hai ki aisi matches kitni jaldi execute hoti hain.
static void BM_LimitMatch(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(1, make_sym());
        static Order maker, taker;
        maker = Order{};
        maker.id=1; maker.symbol_id=1; maker.side=Side::Sell;
        maker.type=OrderType::Limit; maker.price=to_price(100.0);
        maker.qty=to_quantity(1.0); maker.remaining_qty=to_quantity(1.0);
        maker.tif=TimeInForce::GTC; maker.status=OrderStatus::New;
        book.add_limit_order(&maker);

        taker = Order{};
        taker.id=2; taker.symbol_id=1; taker.side=Side::Buy;
        taker.type=OrderType::Limit; taker.price=to_price(100.0);
        taker.qty=to_quantity(1.0); taker.remaining_qty=to_quantity(1.0);
        taker.tif=TimeInForce::GTC; taker.status=OrderStatus::New;
        state.ResumeTiming();
        benchmark::DoNotOptimize(book.add_limit_order(&taker));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LimitMatch);

// ─── Market order full sweep ---------------------------------------------
// basically jab ek iceberg(bht bada order) order book mein aata hai jo kaafi sare ask orders ko match karke clear(sweep) karta hai toh usko pura clear karne mein kitna time lagta hai uske lie ye benchmark hai
static void BM_MarketOrderSweep(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(1, make_sym());
        static Order asks[10], mo;
        for (int i = 0; i < 10; ++i) {
            asks[i] = Order{}; asks[i].id=i+1; asks[i].symbol_id=1;
            asks[i].side=Side::Sell; asks[i].type=OrderType::Limit;
            asks[i].price=to_price(100.0+i*0.01);
            asks[i].qty=to_quantity(0.1); asks[i].remaining_qty=to_quantity(0.1);
            asks[i].tif=TimeInForce::GTC; asks[i].status=OrderStatus::New;
            book.add_limit_order(&asks[i]);
        }
        mo = Order{}; mo.id=100; mo.symbol_id=1; mo.side=Side::Buy;
        mo.type=OrderType::Market; mo.qty=to_quantity(1.0);
        mo.remaining_qty=to_quantity(1.0); mo.status=OrderStatus::New;
        state.ResumeTiming();
        benchmark::DoNotOptimize(book.add_market_order(&mo));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MarketOrderSweep);

// ─── Cancel order ----------------------------------------------------------
// Yeh benchmark check karta hai ki jab koi order book se cancel hota hai, toh usmein kitna time lagta hai
static void BM_CancelOrder(benchmark::State& state) {
    OrderBook book(1, make_sym());
    std::vector<Order> orders(state.range(0));
    for (int i = 0; i < state.range(0); ++i) {
        orders[i] = Order{};
        orders[i].id=i+1; orders[i].symbol_id=1; orders[i].side=Side::Buy;
        orders[i].type=OrderType::Limit; orders[i].price=to_price(100.0-i*0.01);
        orders[i].qty=to_quantity(1.0); orders[i].remaining_qty=to_quantity(1.0);
        orders[i].tif=TimeInForce::GTC; orders[i].status=OrderStatus::New;
        book.add_limit_order(&orders[i]);
    }
    int idx = 0;
    for (auto _ : state) {
        if (idx < state.range(0))
            benchmark::DoNotOptimize(book.cancel_order(idx + 1));
        ++idx;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CancelOrder)->Arg(1000)->Arg(10000);

// ─── Top-of-book snapshot ---------------------------------------------
// top of book basically best bid and best ask price and quantity hoti hai to ye benchmark check karta hai ki is snapshot ko lene mein kitna time lagta hai
static void BM_TopOfBook(benchmark::State& state) {
    OrderBook book(1, make_sym());
    static Order b, a;
    b=Order{}; b.id=1;b.symbol_id=1;b.side=Side::Buy;b.type=OrderType::Limit;
    b.price=to_price(99.0);b.qty=to_quantity(1.0);b.remaining_qty=to_quantity(1.0);b.status=OrderStatus::New;
    a=Order{}; a.id=2;a.symbol_id=1;a.side=Side::Sell;a.type=OrderType::Limit;
    a.price=to_price(101.0);a.qty=to_quantity(1.0);a.remaining_qty=to_quantity(1.0);a.status=OrderStatus::New;
    book.add_limit_order(&b); book.add_limit_order(&a);

    for (auto _ : state) {
        benchmark::DoNotOptimize(book.top_of_book());
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_TopOfBook);

BENCHMARK_MAIN();
