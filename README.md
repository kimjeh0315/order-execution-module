# Order Execution Module

A low-latency crypto order execution module written in C++17.  
Receives computed positions from an alpha model and dispatches orders to CEX/DEX gateways with **zero alpha decay** — the hot path is designed to return in **~microseconds**, never blocking on I/O.

---

## Architecture

```
Alpha Model
    │
    │ OrderRequest
    ▼
┌─────────────────────────────────────────────────────────────┐
│                       OrderExecutor                          │
│                                                             │
│  receiveOrder()  ◄──────────────────────────────────────┐  │
│       │                                                  │  │
│   ┌───▼──────────────────────┐                          │  │
│   │  SpinLock (critical sec) │  ~30 ns hold time        │  │
│   │  ① Stale signal check    │  timestamp vs now_ms     │  │
│   │  ② validateOrder()       │  cash / oversell guard   │  │
│   │  ③ Margin deduction      │  notional / leverage     │  │
│   │  ④ Gateway ID resolve    │  O(1) array lookup       │  │
│   └───────────────┬──────────┘                          │  │
│                   │ SpinLock released                   │  │
│                   ▼                                     │  │
│   ┌───────────────────────────┐                         │  │
│   │  std::visit (AnyGateway)  │  static dispatch        │  │
│   │  BinanceGateway           │  no vtable              │  │
│   │  HyperliquidGateway       │                         │  │
│   └───────────┬───────────────┘                         │  │
│               │ pool.submit(Task)                        │  │
│               ▼                                         │  │
│   ┌───────────────────────────┐                         │  │
│   │     MPMCQueue<Task, 64>   │  lock-free ring buffer  │  │
│   └───────────┬───────────────┘                         │  │
│               │                                         │  │
│   ┌───────────▼───────────────┐                         │  │
│   │  ThreadPool (10 workers)  │  pre-spawned, spinning  │  │
│   │  worker_fn() → task()     │  sends to exchange      │  │
│   └───────────────────────────┘                         │  │
│                                                         │  │
│  onOrderExecuted() ─────────────────────────────────────┘  │
│  (fill confirmed → update FlatHashMap position book)        │
│                                                             │
│  AsyncLogger (MPSC ring + I/O thread) — never blocks hot   │
└─────────────────────────────────────────────────────────────┘
```

---

## Optimizations

### 1. Thread Pool — Eliminating Thread Creation Overhead

**Problem:** `std::thread().detach()` per order costs several milliseconds (kernel stack allocation, scheduler registration).  
**Solution:** 10 worker threads are pre-spawned at startup. Orders are submitted as `Task` objects into a lock-free queue. Thread creation cost is paid once, at initialization.

### 2. SpinLock — Avoiding OS Sleep on the Hot Path

**Problem:** `std::mutex` suspends threads via `futex(WAIT)` — context switching adds 1–10 µs.  
**Solution:** `std::atomic_flag`-based `SpinLock`. For the ~30 ns critical section (cash arithmetic + map lookup), spinning is cheaper than sleeping. The SpinLock scope is deliberately narrow: it is released *before* calling into the gateway.

### 3. Lock-Free MPMC Queue — Contention-Free Task Dispatch

**Problem:** A mutex-protected `std::queue` inside the thread pool creates a shared lock hotspot.  
**Solution:** Dmitry Vyukov's MPMC ring buffer (`MPMCQueue<Task, 64>`). Each producer CAS-advances the enqueue cursor independently; consumers do the same for the dequeue cursor. No lock is held during data transfer.

### 4. Heap-Free `Task` — Eliminating `malloc` on the Hot Path

**Problem:** `std::function` performs a heap allocation when the captured lambda exceeds its internal SBO buffer (~24 bytes). `OrderRequest` is 64 bytes — always heap-allocated.  
**Solution:** Custom `Task` struct with `alignas(64)` inline storage (128 bytes). Lambdas are placement-new'd directly into this buffer. Zero `malloc` calls on the submission path.

### 5. Async Logger — I/O Never Blocks the Hot Path

**Problem:** `std::cout` acquires an internal mutex and calls `write()`. Even `printf` is not async-signal-safe.  
**Solution:** MPSC ring buffer (`AsyncLogger`). `pushf()` writes formatted output into a slot and marks it ready with an atomic store. A dedicated I/O thread drains the ring and calls `fwrite()`. The hot path never touches a file descriptor.

### 6. FlatHashMap — Cache-Friendly Position Book

**Problem:** `std::unordered_map` uses chained buckets — each lookup involves pointer chasing across heap-allocated nodes.  
**Solution:** Open-addressing flat hash map (`FlatHashMap<PosKey, Position, PosKeyHash, 512>`) with linear probing. The entire table fits in L2 cache. Capacity is fixed at compile time — no rehashing, no heap.

### 7. Static Gateway Dispatch — Zero vtable Overhead

**Problem:** `virtual sendOrderAsync()` costs one indirect branch (vtable lookup) per order.  
**Solution:** `std::variant<BinanceGateway, HyperliquidGateway>` + `std::visit`. The compiler generates a jump table from a known set of types, resolved at compile time. No vtable pointer, no heap allocation for polymorphic storage.

### 8. Stack-Allocated Keys — No String Heap Allocation

**Problem:** `std::string` concatenation for position map keys (`"0_1_ETHUSDT"`) calls `malloc` on every lookup.  
**Solution:** `PosKey` is a `char[24]` stack buffer. `makePosKey()` uses `std::to_chars` (no format-string parsing) and manual character copying. Key construction is ~5 ns with zero heap traffic.

### 9. Overflow-Safe Notional Calculation

**Problem:** `price_ticks * qty_ticks` (`price × SCALE × qty × SCALE`) overflows `int64_t` for typical crypto prices (e.g., BTC 70,000 × qty 1 → 7×10²⁰ >> 9.2×10¹⁸).  
**Solution:** `(price_ticks / SCALE) * qty_ticks` — divide first, then multiply. Integer-exact for whole-number prices, no precision loss, no overflow.

### 10. Instrument-Aware SELL Validation — Correct Short Selling Semantics

**Problem:** Treating all SELL orders identically (inventory check) is correct for SPOT but fatally wrong for FUTURES/OPTIONS. Rejecting a FUTURES short because "no coins owned" would block the module's primary revenue path for bearish alpha signals.  
**Solution:** `validateOrder` branches on `inst_type`:
- **SPOT** (`inst_type = 0`): Must own the asset. Returns `REJECT_OVERSELL` if inventory is insufficient.
- **FUTURES / OPTIONS** (`inst_type = 1, 2`): Short selling is allowed. Checks initial margin (`notional / leverage`) against cash, identical to a BUY order.

This also aligns `onOrderExecuted` settlement correctly:
- **SPOT SELL fill**: Returns full proceeds to `cash_ticks` immediately (spot settles T+0 or T+1).
- **FUTURES / OPTIONS SELL fill**: Only updates the position book (qty goes negative = short open). Margin return and P&L settlement are delegated to the external risk system via `updateUnrealizedProfit()`.

### 11. Zero-Allocation Payload Serialization (Future-Proofing for Real Requests)

**Problem:** When replacing gateway stubs with real REST/WebSocket calls, JSON generation libraries (e.g., `nlohmann/json`) or `std::string` concatenation introduce heap allocations on the hot path.  
**Solution:** When productionizing the gateway `sendOrderAsync` stubs, build the JSON/query payload into a stack-allocated `char[512]` buffer using `std::to_chars` and manual string copying (or `simdjson`'s serialization API / `fmt::format_to` with a stack buffer). This keeps zero-allocation guarantees end-to-end.

---

## Components

| Component | File | Role |
|---|---|---|
| `SpinLock` | `OrderExecutor.h` | Lightweight critical-section lock |
| `Task` | `OrderExecutor.h` | Heap-free, fixed-size callable (128 B inline storage) |
| `MPMCQueue<T, N>` | `OrderExecutor.h` | Lock-free ring buffer (Vyukov algorithm) |
| `AsyncLogger` | `OrderExecutor.h` | MPSC ring + background I/O thread |
| `ThreadPool` | `OrderExecutor.h` | 10 pre-spawned spinning workers |
| `FlatHashMap<K,V,H,N>` | `OrderExecutor.h` | Open-addressing hash map, zero heap |
| `makePosKey()` | `OrderExecutor.h` | Stack-allocated composite key builder |
| `OrderRequest` | `OrderExecutor.h` | 64-byte input struct from alpha model |
| `BinanceGateway` | `OrderExecutor.cpp` | CEX adapter (REST/WebSocket stub) |
| `HyperliquidGateway` | `OrderExecutor.cpp` | DEX adapter (EIP-712 signing stub) |
| `OrderExecutor` | `OrderExecutor.cpp` | Core orchestrator |
| `DummyOrderGenerator` | `dummyordergenerator.h` | Test fixture factory (heap-free `std::array`) |

---

## `OrderRequest` — Input Contract

The alpha model fills this struct and passes it to `receiveOrder()`. The executor trusts all fields without re-validation of business logic.

```cpp
struct OrderRequest {
    int     order_id;
    int     exchange_id;       // index into gateway routing table
    int     inst_type;         // 0=SPOT, 1=FUTURES, 2=OPTIONS
    int     leverage;          // margin multiplier (alpha model decides)
    char    symbol[16];
    int     side;              // 0=BUY, 1=SELL
    int     max_signal_age_ms; // 0=no check; >0=reject if signal is stale
    int64_t price_ticks;       // price × SCALE (SCALE = 1e8)
    int64_t qty_ticks;         // quantity × SCALE
    int64_t timestamp_ms;      // Unix ms when the alpha signal was generated
    // sizeof = 64 B — fits exactly in Task inline storage
};
```

**Margin formula (BUY):**
```
notional = (price_ticks / SCALE) × qty_ticks
margin   = notional / leverage
```
SPOT (lev=1) requires full notional. FUTURES (lev=10) requires 10% margin. The executor does not impose any leverage cap — that is the alpha model's responsibility.

---

## Portfolio State

```
Total Portfolio Value = cash + unrealized_profit + realized_profit
```

This module manages **cash** and **unrealized profit** only.

| State | Owner | Notes |
|---|---|---|
| `cash_ticks` | `OrderExecutor` | Decremented on BUY dispatch, incremented on SELL fill |
| `unrealized_profit_ticks` | Set via `updateUnrealizedProfit()` | Read-only for logging; **never used for margin** |
| Position book | `FlatHashMap` (512 slots) | `qty_ticks` per `(exchange_id, inst_type, symbol)` key |

Unrealized profit is intentionally excluded from margin calculations. It can evaporate instantly with market movement and must not be used to fund new positions.

---

## Test Cases

### Category A — Speed and Concurrency

#### TC1: Next-Tick Sequential Orders (Zero-Blocking)

**Scenario:** Symbol A's position is computed. 1 ms later, Symbol B's BUY arrives.  
**Mechanism:** Each `receiveOrder()` call dispatches into the `MPMCQueue` and returns **before** the gateway's network I/O completes. Binance simulates 50 ms latency; the hot path completes in < 10 µs.  
**Verification:** `high_resolution_clock` measures each dispatch. Both should be in the **single-digit microsecond** range.

```
[TC1-Timing] BTCUSDT dispatch: 3us | SOLUSDT dispatch: 2us (gateway I/O is async — not blocking)
```

#### TC2: Concurrent Multi-Symbol Orders (SpinLock Contention)

**Scenario:** Alpha model signals BTC, ETH, and ADA at the exact same millisecond.  
**Mechanism:** Three threads call `receiveOrder()` simultaneously. The SpinLock serializes book updates. The MPMC queue absorbs all three tasks without a mutex.  
**Verification:** All three orders are dispatched; no order is lost or corrupted.

---

### Category B — Position and Capital Defense

#### TC3: Buy Pending → Sell Conflict (Oversell Guard)

**Scenario:** ETH FUTURES BUY is dispatched but not yet confirmed by the exchange. The alpha model fires an ETH SELL signal.  
**Mechanism:** `validateOrder()` checks `FlatHashMap` — the position is still zero (confirmation not received). Returns `REJECT_OVERSELL`.  
**After:** `onOrderExecuted()` is called with the late BUY confirmation. Position book is updated.

```
[CRITICAL REJECT] SELL before BUY confirmed. Order: 302, Symbol: ETHUSDT
[System] BUY Filled. +10 [1_1_ETHUSDT]
```

#### TC4: Real-Time Cash Validation (Unrealized Profit Exclusion)

**Scenario:** `cash = 100,000`, `unrealized_profit = 20,000` (total portfolio = 120,000). A 150,000 USDT BUY order arrives.  
**Mechanism:** `validateOrder()` compares `required (150,000)` against `cash_ticks (100,000)` only. Unrealized profit is visible in the log but does not extend buying power.  
**Result:** `REJECT_CASH`.

```
[Reject] REJECT_CASH. Order 401 | required=150000 | cash=100000 | unrealized=20000
```

---

### Category C — Live Trading Defense

#### TC5: Stale Signal Rejection

**Scenario:** Alpha model generates a SOL BUY at time T. OS lag causes it to arrive at the executor at T + 2,000 ms. The alpha model has set `max_signal_age_ms = 500`.  
**Mechanism:** `get_now_ms()` is captured **before** the SpinLock. Inside `validateOrder()`, `now - timestamp_ms = ~2000 ms > 500 ms` → `Verdict::STALE`. The order never touches the position book or gateway.

```
[Stale] Signal expired. Order 501 | age=2003ms > limit=500ms
```

---

## Build Instructions

### Prerequisites

| Tool | Minimum Version |
|---|---|
| CMake | 3.16 |
| C++ Compiler | GCC 9 / Clang 10 / MSVC 2019 (C++17) |

---

### Linux / macOS (GCC or Clang)

```bash
# Clone / navigate to the module directory
cd order_execute_module

# Configure (Release)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Run
./build/order_executor_test
```

**With AddressSanitizer (memory safety check):**
```bash
cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build_asan -j$(nproc)
./build_asan/order_executor_test
```

**With ThreadSanitizer (data race detection):**
```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build build_tsan -j$(nproc)
./build_tsan/order_executor_test
```

---

### Windows (MSVC — Visual Studio 2019/2022)

```powershell
# In the module directory
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Run
.\build\Release\order_executor_test.exe
```

**Or with the CMake + MSVC command-line toolchain:**
```powershell
# Open "x64 Native Tools Command Prompt for VS 2022"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\order_executor_test.exe
```

---

### Windows (MinGW / MSYS2)

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/order_executor_test.exe
```

---

## Expected Output

```
========== [Order Execution Module: Core Test Cases] ==========

[System] ThreadPool ready. 10 spinning workers, lock-free queue (cap=64).
[System] Executor Ready. Cash: 30000000000000 ticks
[System] Gateway registered for Exchange ID: 0
[System] Gateway registered for Exchange ID: 1

[TC1] Next-Tick Sequential Orders (Zero-Blocking)
[TC1-Timing] BTCUSDT dispatch: 3us | SOLUSDT dispatch: 2us (gateway I/O is async — not blocking)

[TC2] Concurrent Multi-Symbol Orders (3 threads, same millisecond)
[CEX-Binance] Order Dispatched. ID: 201 (Latency: ~50ms)
[CEX-Binance] Order Dispatched. ID: 202 (Latency: ~50ms)
[CEX-Binance] Order Dispatched. ID: 203 (Latency: ~50ms)

[TC3] Buy Pending → Sell Conflict (REJECT_OVERSELL expected)
[CRITICAL REJECT] SELL before BUY confirmed. Order: 302, Symbol: ETHUSDT
[Network] Late BUY confirmation arrives for Order 301.
[System] BUY Filled.  +10 [1_1_ETHUSDT]

[TC4] Cash Validation with Unrealized Profit (REJECT_CASH expected)
[System] Gateway registered for Exchange ID: 0
[Reject] REJECT_CASH. Order 401 | required=150000 | cash=100000 | unrealized=20000

[TC5] Stale Signal Rejection (STALE expected, age=~2000ms > 500ms)
[Stale] Signal expired. Order 501 | age=2003ms > limit=500ms

========== [All Test Cases Completed] ==========
```

> Gateway I/O confirmation messages (CEX-Binance / DEX-Hyperliquid) may appear out of order due to async execution — this is expected behavior.

---

## File Structure

```
order_execute_module/
├── CMakeLists.txt          # Build system
├── README.md               # This file
├── OrderExecutor.h         # All core data structures and class declarations
├── OrderExecutor.cpp       # OrderExecutor, gateway implementations
├── dummyordergenerator.h   # Test fixture factory (TC1–TC5)
└── main.cpp                # Test entry point
```

---

## Performance Characteristics

| Path | Estimated Latency |
|---|---|
| `receiveOrder()` hot path (validation + dispatch) | < 1 µs (no I/O, no heap) |
| SpinLock hold time (critical section) | ~30 ns |
| `AsyncLogger::pushf()` | ~20 ns (atomic fetch-add + memcpy) |
| `FlatHashMap::find()` / `operator[]` | ~5–15 ns (L1/L2 cache hit) |
| `makePosKey()` | ~5 ns (`std::to_chars` + manual copy) |
| Thread pool task submission (`MPMCQueue::enqueue`) | ~10 ns (single CAS) |
| Gateway network I/O (Binance stub) | ~50 ms (async, not on hot path) |
| Gateway network I/O (Hyperliquid stub) | ~1,050 ms (async, not on hot path) |

All latency-critical operations avoid: `malloc`, `free`, `mutex`, `std::string`, virtual dispatch, and synchronous I/O.

---

## Known Limitations & Production Gaps

This section documents what exists in the current implementation versus a fully production-ready module. These are known trade-offs, not bugs.

### Critical — Must Address Before Live Trading

| Gap | Current State | What a Production Module Needs |
|---|---|---|
| **No real network code** | `sleep_for` simulates latency | Actual HMAC-SHA256 signed REST calls (Binance) or EIP-712 typed-data signing + JSON-RPC (Hyperliquid), with TLS socket management |
| **No `order_type` field** | All orders treated as limit (price specified) | `order_type` enum: `LIMIT`, `MARKET`, `IOC`, `FOK`, `STOP_LIMIT`. Market orders have no price; IOC/FOK have fill-or-kill semantics that affect gateway serialization entirely |
| **No Time-in-Force (TIF)** | Orders live indefinitely once sent | `GTC` (Good-Till-Cancelled), `IOC` (Immediate-or-Cancel), `FOK` (Fill-or-Kill). IOC is the standard for HFT to avoid stale resting orders |
| **No partial fill handling** | `onOrderExecuted` assumes 100% fill | `onOrderExecuted` should accept a `fill_qty` and `fill_price` parameter. Position and cash must be updated proportionally |
| **No order cancellation** | No `cancelOrder()` | If market conditions change before fill, a pending order must be cancellable via the gateway's cancel API |
| **No fill price** | Uses order's original price | `onOrderExecuted` should receive the actual execution price from the exchange, which can differ from the order price (slippage, partial fills) |

### Significant — Needed for Derivatives

| Gap | Current State | What a Production Module Needs |
|---|---|---|
| **FUTURES P&L settlement** | Margin held indefinitely after short open; never returned | Track open vs close orders (or net position direction). Return margin + realized P&L when a position is closed. Currently delegated to external risk system |
| **OPTIONS margin model** | Treated identically to FUTURES | Options have non-linear P&L profiles. Writing (selling) options requires a different margin model (e.g., SPAN margin). Delta/vega-based margin is outside scope of this module but the `inst_type` field enables future differentiation |
| **No close-vs-open distinction** | FUTURES short and close-long both deduct margin | A `reduce_only` flag or position direction tracking would allow the module to skip re-margining on close orders |

### Minor — Robustness

| Gap | Current State | What a Production Module Needs |
|---|---|---|
| **No duplicate order ID guard** | Same `order_id` processed twice will double-deduct cash | A `std::atomic`-protected seen-ID bitset or generation counter |
| **No position limits** | Unlimited position size | Max notional or position size per symbol/instrument, checked in `validateOrder` |
| **Hardcoded gateway capacity** | `MAX_EXCHANGES = 8` (compile-time constant) | Runtime-configurable routing table; dynamic gateway registration from config file |
| **No reconnect / failover** | Single gateway per exchange ID | Production gateways require heartbeat monitoring, automatic reconnect, and primary/backup failover |

### Design Boundary (Intentional Scope)

The following are explicitly **out of scope** for an order execution module and belong to upstream/downstream systems:

- **Alpha signal generation** — This module is a pure executor; signal logic lives in the alpha model.
- **Realized P&L tracking** — Computed by the portfolio/risk layer, injected via `updateUnrealizedProfit()`.
- **Risk limits (drawdown, VaR)** — Enforced by the risk system before signals reach this module.
- **Order book / market data** — Not consumed here; pricing is supplied by the alpha model in `OrderRequest`.
