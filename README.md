# High-Performance C++ Order Book Engine

A production-grade, lock-free, multi-symbol order matching engine targeting **sub-microsecond** matching latency.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                       Strategy Engine                            │
│            Buy Signal / Sell Signal / Flat Signal                │
└──────────────────────┬───────────────────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│                      Order Generator                             │
│          Converts signals → typed Order structs                  │
│          Runs TWAP / VWAP slicing / SOR routing                  │
└──────────────────────┬───────────────────────────────────────────┘
                       │ Lock-free MPSC queue
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Matching Engine                               │
│  ┌─────────────────────┐  ┌─────────────────────┐              │
│  │  BTC-USDT Worker    │  │  ETH-USDT Worker    │  (per symbol)│
│  │  (dedicated thread) │  │  (dedicated thread) │              │
│  │  ┌───────────────┐  │  │  ┌───────────────┐  │              │
│  │  │  Order Book   │  │  │  │  Order Book   │  │              │
│  │  │  Bids (desc)  │  │  │  │  Bids (desc)  │  │              │
│  │  │  Asks (asc)   │  │  │  │  Asks (asc)   │  │              │
│  │  │  Stop Map     │  │  │  │  Stop Map     │  │              │
│  │  └───────────────┘  │  │  └───────────────┘  │              │
│  └─────────────────────┘  └─────────────────────┘              │
└──────────────────────┬───────────────────────────────────────────┘
                       │ Event Bus (per-subscriber SPSC queues)
          ┌────────────┼────────────┬─────────────┐
          ▼            ▼            ▼             ▼
   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
   │  Risk    │ │Portfolio │ │Execution │ │ Feature  │
   │  Engine  │ │ Engine   │ │ Engine   │ │ Engine   │
   └──────────┘ └──────────┘ └──────────┘ └──────────┘
                                   │
                                   ▼
                          ┌──────────────────┐
                          │  Real Exchange   │
                          │  FIX / REST / WS │
                          └──────────────────┘
```

## Features

### Basic
- [x] Limit orders (buy/sell)
- [x] Market orders
- [x] Order matching (price-time priority FIFO)
- [x] Partial fills
- [x] Trade generation
- [x] Best bid/ask
- [x] Spread calculation

### Medium
- [x] Stop loss orders (trigger on price crossing)
- [x] Take profit orders
- [x] Order cancel (O(1) from book)
- [x] Order modify (cancel + re-insert)
- [x] Iceberg orders (hidden reserve, display qty)
- [x] Hidden orders (not shown in depth)
- [x] Post-only orders (reject if would cross spread)

### Advanced
- [x] Multi-symbol order book (one thread per symbol)
- [x] Multi-thread matching engine (zero inter-symbol contention)
- [x] Lock-free structures (SPSC/MPSC ring buffers, object pool)
- [x] Price-time priority (FIFO queues per price level)
- [x] Order imbalance detection
- [x] Liquidity gap detection

### Hard
- [x] TWAP orders (equal time slices)
- [x] VWAP orders (volume-weighted slices from candle data)
- [x] Smart Order Router (minimize slippage across symbols/venues)
- [x] Market making support (two-sided quotes, inventory skew)
- [x] Slippage estimation (walk-the-book simulation)
- [x] Queue position tracking (ahead qty estimation)

## Data Structures

| Component | Data Structure | Complexity |
|---|---|---|
| Price levels | `std::map<Price, PriceLevelQueue>` | O(log N) insert/lookup |
| Order FIFO queue | `std::list<Order*>` per level | O(1) push/pop |
| Order lookup | `std::unordered_map<OrderId, OrderEntry>` | O(1) cancel |
| Stop triggers | `std::map<Price, vector<Order*>>` | O(log N) trigger check |
| Inter-thread | `SPSCQueue<T, N>` / `MPSCQueue<T, N>` | O(1) wait-free |
| Order memory | `ObjectPool<Order, 1M>` | O(1) no-heap alloc |

## Project Structure

```
order-book/
├── CMakeLists.txt
├── src/
│   ├── core/
│   │   ├── types.hpp           # Scaled int price/qty, enums
│   │   ├── order.hpp           # Order, Trade, ExecutionReport structs
│   │   ├── lock_free_queue.hpp # SPSC + MPSC ring buffers
│   │   ├── object_pool.hpp     # Lock-free pre-allocated order pool
│   │   └── event_bus.hpp       # Fan-out event bus
│   ├── book/
│   │   ├── price_level.hpp     # FIFO queue per price level
│   │   ├── order_book.hpp      # Single-symbol book + matching engine
│   │   └── slippage_estimator.hpp
│   ├── engine/
│   │   ├── matching_engine.hpp # Multi-symbol, multi-thread dispatcher
│   │   ├── twap_vwap.hpp       # TWAP / VWAP algo executors
│   │   ├── smart_order_router.hpp
│   │   └── market_maker.hpp
│   ├── downstream/
│   │   ├── risk_engine.hpp
│   │   ├── portfolio_engine.hpp
│   │   └── execution_engine.hpp
│   └── main.cpp
├── tests/
│   └── test_order_book.cpp
└── benchmarks/
    └── bench_order_book.cpp
```

## Build

### Prerequisites
- GCC 12+ or Clang 15+ (C++20)
- CMake 3.20+
- Linux / WSL2 recommended for lowest latency

### Build & Run

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Run demo
./build/src/order_book_engine

# Run tests
./build/tests/test_order_book

# Run benchmarks
./build/benchmarks/bench_order_book --benchmark_format=console
```

### Debug build (with ASan + UBSan)
```bash
cmake -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug -j$(nproc)
./build_debug/src/order_book_engine
```

## Performance Targets

| Operation | Target Latency |
|---|---|
| Limit order insert (no match) | < 200 ns |
| Limit order full match | < 500 ns |
| Market order (10-level sweep) | < 1 µs |
| Cancel order | < 100 ns |
| Top-of-book snapshot | < 50 ns |
| SPSC queue round-trip | < 50 ns |

## Integration with Existing System

The engine consumes market data from:
- **Redis Streams** → real-time tick data → price reference for SOR/slippage
- **PostgreSQL** → historical candle OHLCV → volume profiles for VWAP

It produces:
- **Trade events** → Risk Engine, Portfolio Engine
- **Execution Reports** → Execution Engine → Real Exchange
- **Top-of-Book events** → Feature Engine, Strategy Engine

## Thread Model

```
Thread 1 (Strategy)  ──MPSC──► Thread N+1 (BTC-USDT Matching)
Thread 2 (Strategy)  ──MPSC──►
...                            ──EventBus (SPSC)──► Thread M (Risk)
Thread K (Strategy)  ──MPSC──► Thread N+2 (ETH-USDT Matching)
                               ──EventBus (SPSC)──► Thread M+1 (Portfolio)
```

No mutexes on the hot path. All cross-thread communication is lock-free.
