# FLOX Comprehensive Q&A — C++ & Python Usage Guide

> Answers to Q1–Q13+ based on thorough reading of all FLOX documentation
> (architecture, APIs, specs, tutorials, demos, examples, Python bindings).
>
> Generated: 2026-05-09

---

## Table of Contents

- [Q1: Where to put custom C++ code — inside FLOX fork or separate repo?](#q1)
- [Q2: Does FLOX require tick data, or can it work with 1M bars?](#q2)
- [Q3: Using CSV / Parquet broker data with FLOX](#q3)
- [Q4: How FLOX indicators are calculated — tick-level or bar-level?](#q4)
- [Q5: Historical tick data availability from brokers](#q5)
- [Q6: Incremental data updates — deltas and merging](#q6)
- [Q7: Live trading data subscription, warmup, and Oryon comparison](#q7)
- [Q8: Oryon (Rust) integration with FLOX C++ strategies](#q8)
- [Q9: Feeding data to backtesting — formats and Indicator Graph](#q9)
- [Q10: Live trading — metrics storage, memory, warmup best practices](#q10)
- [Q11: Trading entities explained — orders, books, positions, and their relationships](#q11)
- [Q11b: Accessing previous bars in FLOX strategies](#q11b)
- [Q12: Strategy behavior — on_bar vs intrabar, batch vs streaming](#q12)
- [Q13: Complex exit strategies — ATR stops, Chandelier, trailing, and exit alphas](#q13)

---

<a id="q1"></a>
## Q1: Where to put custom C++ code — inside FLOX fork or separate repo?

**Recommendation: Start inside the FLOX fork. Move to a separate repo later if needed.**

### Why inside the fork is easier initially:

1. **Indicator auto-registration**: New indicators added to `include/flox/indicator/` with one line in `registry.def` automatically surface in Python, Node, Codon, and QuickJS bindings. This only works when your code is part of the FLOX build tree.

2. **CMake integration**: FLOX's CMake build system handles all includes, linking, and binding generation. Your strategies and indicators compile as part of the normal `cmake --build` flow without custom build scripts.

3. **Header-only flexibility**: Strategies don't need to be in the FLOX tree — you can include FLOX headers from anywhere. But indicators and custom subsystems benefit from being inside the tree.

### Recommended directory structure inside the fork:

```
vendor/flox/
├── include/flox/indicator/          # Add new indicators here
│   ├── my_atr_stop.h
│   └── my_chandelier.h
├── src/indicator/
│   └── registry.def                 # Add one line per indicator
├── _my/                             # YOUR custom code (not part of upstream FLOX)
│   ├── strategies/                  # C++ strategy implementations
│   │   ├── donchian_strategy.h
│   │   ├── donchian_strategy.cpp
│   │   ├── dual_momentum_strategy.h
│   │   └── ...
│   ├── grid_searches/               # Grid backtest runners
│   │   ├── donchian_grid.cpp
│   │   └── ...
│   ├── data/                        # Downloaded market data
│   │   └── binance/
│   └── crush-glm/                   # This document
├── demo/                            # FLOX's own demos (upstream)
└── docs/                            # FLOX's own docs (upstream)
```

### When to use a separate repo:

- When you have multiple projects sharing the same strategies
- When you need CI/CD independent of FLOX
- When you want to version your strategies separately from FLOX engine updates

To use a separate repo, you'd:
1. Install FLOX as a CMake dependency (via `FetchContent` or `find_package`)
2. Include FLOX headers: `#include <flox/strategy/strategy.h>`
3. Link against `flox::flox` target

But this requires more build plumbing. Start inside the fork.

### For Python-only strategies:

Python strategies don't need to be inside the FLOX tree at all. Just `import flox_py` and write strategies anywhere. However, if you want your indicators to work in both C++ and Python, they must be C++ inside the FLOX tree.

---

<a id="q2"></a>
## Q2: Does FLOX require tick data, or can it work with 1M bars?

**FLOX can work with both tick data and pre-aggregated bars. You do NOT need tick data.**

### Three data paths in FLOX:

| Path | Input | How | Best For |
|------|-------|-----|----------|
| **CSV bars** | `data.csv` with OHLCV columns | `BacktestRunner::run_csv()` | Quick backtests, broker data |
| **Pre-aggregated bars** | `bars_60s.bin` (MmapBarStorage) | `BacktestRunner::run_bars()` | High-performance backtests |
| **Tick replay** | `.floxlog` binary tape | `BacktestRunner::run()` with ReplayConnector | Tick-level fidelity, realistic fills |

### Using 1M bars directly:

```cpp
// Option 1: CSV (simplest)
runner.run_csv("btcusdt_1m.csv", strategy, executor);

// Option 2: Pre-aggregated binary bars (fastest)
auto storage = MmapBarStorage::open("bars_60s.bin");
runner.run_bars(storage, strategy, executor);
```

### From Python:

```python
# CSV backtest
runner = flox_py.BacktestRunner()
result = runner.run_csv("btcusdt_1m.csv", strategy, executor)

# Binary bar backtest
storage = flox_py.MmapBarStorage.open("bars_60s.bin")
result = runner.run_bars(storage, strategy, executor)
```

### Converting 1M CSV to FLOX binary bars:

Use the `preagg_bars` CLI tool if you have `.floxlog` data, or write a simple converter:

```cpp
// Convert CSV → MmapBarStorage
#include <flox/backtest/mmap_bar_storage.h>
#include <flox/aggregator/bar.h>

void csv_to_mmap(const std::string& csv_path, const std::string& out_path) {
    MmapBarStorage storage(out_path, /*create=*/true);
    // Read CSV rows, create Bar structs, write to storage
    // Bar has: open, high, low, close, volume, buyVolume,
    //          tradeCount, startTime, endTime, reason
    storage.write_bar(bar);
}
```

### What you lose without tick data:

- **Intrabar fills**: With bars, all fills happen at open/high/low/close. With ticks, fills happen at exact trade prices.
- **Queue simulation**: `QueueSimulationMode::TOB` and `FULL` require tick-level order book data.
- **Realistic slippage**: Volume-impact slippage model works better with tick data.

For most systematic strategies that trade on bar close, **1M bars are sufficient**.

---

<a id="q3"></a>
## Q3: Using CSV / Parquet broker data with FLOX

### Direct CSV usage:

`BacktestRunner::run_csv()` accepts CSV files directly. Expected columns:

```csv
timestamp,open,high,low,close,volume
2020-01-01T00:00:00Z,7200.5,7215.3,7198.1,7210.2,1234.56
```

The CSV reader handles standard OHLCV format. Timestamp parsing is flexible (ISO 8601, Unix timestamp).

### From Python:

```python
# CSV works out of the box
result = runner.run_csv("broker_data.csv", strategy, executor)
```

### Converting CSV to FLOX native formats:

**Why convert?** Binary formats are 10-50x faster to load than CSV for large datasets.

#### Format 1: MmapBarStorage (`bars_*.bin`)

Binary format: `[uint64 bar_count][Bar × N]` where each `Bar` contains:
- `double open, high, low, close`
- `double volume, buyVolume`
- `uint64_t tradeCount`
- `int64_t startTime, endTime` (epoch nanoseconds)
- `uint32_t reason` (bar close reason)

Zero-copy memory-mapped access — the OS pages data directly into your process.

#### Format 2: `.floxlog` (tape format)

Binary tape with segments, frames, and records:
- **SegmentHeader** (64B): exchange, symbol, timeframe metadata
- **FrameHeader** (12B): timestamp, record type, record count
- **TradeRecord** (48B): price, quantity, side, timestamp, trade_id
- **BookRecordHeader** (40B) + **BookLevel** (16B): order book snapshots

All values in fixed-point (scale 1e8) for deterministic cross-platform replay. CRC32 checksums per segment. Optional LZ4 compression.

### Building bars from 1M candles (not from ticks):

**Yes, this is possible.** Use a custom `BarPolicy`:

```cpp
// Custom bar policy that builds e.g. 4H bars from 1M candles
struct FourHourFromOneMinute {
    bool shouldClose(const Bar& current, int64_t now) const {
        // Close bar every 240 minutes
        return (now / (4 * 3600 * 1'000'000'000LL))
             > (current.startTime / (4 * 3600 * 1'000'000'000LL));
    }
    void update(Bar& bar, double price, double qty, bool is_buy, int64_t ts) {
        bar.high = std::max(bar.high, price);
        bar.low = std::min(bar.low, price);
        bar.close = price;
        bar.volume += qty;
        if (is_buy) bar.buyVolume += qty;
        bar.tradeCount++;
        bar.endTime = ts;
    }
    void initBar(Bar& bar, double price, double qty, bool is_buy, int64_t ts) {
        bar.open = bar.high = bar.low = bar.close = price;
        bar.volume = qty;
        bar.buyVolume = is_buy ? qty : 0;
        bar.tradeCount = 1;
        bar.startTime = bar.endTime = ts;
    }
};

// Then use with BarAggregator
BarAggregator<FourHourFromOneMinute> agg;
```

Or simpler: aggregate 1M CSV into 4H CSV with a Python script, then feed to FLOX.

### Parquet support:

FLOX does NOT natively read Parquet. Convert to CSV first:

```python
import pandas as pd
df = pd.read_parquet("data.parquet")
df.to_csv("data.csv", index=False)
# Then use run_csv()
```

Or write a custom C++ reader using Arrow/Parquet libraries and feed bars directly.

---

<a id="q4"></a>
## Q4: How FLOX indicators are calculated — tick-level or bar-level?

**Indicators are calculated on BARS, not raw ticks.** The pipeline is:

```
Trades/Ticks → BarAggregator → Bars → Indicators
```

### Two modes of indicator computation:

#### 1. Batch mode: `compute(all_bars) → all_values`

```cpp
#include <flox/indicator/sma.h>

std::vector<double> closes = /* extract from bars */;
auto sma_values = flox::indicator::SMA::compute(closes, /*period=*/20);
// Returns vector<double> of same length
```

#### 2. Streaming mode: `update(bar) → value()` (per-bar incremental)

```cpp
flox::indicator::StreamingSMA sma(20);

for (const auto& bar : bars) {
    sma.update(bar.close);
    if (sma.ready()) {
        double current_sma = sma.value();
        // Use for trading decisions
    }
}
```

Every indicator supports both modes via mixins:
- `StreamingSingle<T>` — single-value indicators (SMA, EMA, RSI, etc.)
- `StreamingBar<T>` — bar-based indicators
- `StreamingHighLow<T>` — indicators needing high/low (Bollinger, Keltner)
- `StreamingOhlc<T>` — indicators needing full OHLC
- `StreamingPair<T>` — indicators on two series (Correlation, etc.)

### Should you use the Indicator Graph?

**Yes, almost always.** The Indicator Graph (`flox::indicator::IndicatorGraph`) is a DAG-based compute-once-cache:

```cpp
auto graph = IndicatorGraph("BTCUSDT");

// Add nodes with dependencies
auto& sma20 = graph.add_node("sma_20", [](const auto&) {
    return std::make_shared<StreamingSMA>(20);
});
auto& sma50 = graph.add_node("sma_50", [](const auto&) {
    return std::make_shared<StreamingSMA>(50);
});
auto& cross = graph.add_node("sma_cross", {&sma20, &sma50},
    [](const auto& deps) {
        return std::make_shared<CrossSignal>(deps[0], deps[1]);
    });

// On each bar close:
graph.update(current_bar);

// Read values — computed once, cached
double sma20_val = graph.require("sma_20")->value();
double cross_val = graph.require("sma_cross")->value();
```

**Benefits:**
- **No redundant computation**: SMA(20) used by 3 different conditions is computed once
- **Automatic dependency resolution**: `require("sma_cross")` ensures SMA(20) and SMA(50) are computed first
- **Per-symbol isolation**: Each symbol has its own graph
- **Warmup tracking**: `ready()` propagates through the DAG

### Parity guarantee:

Streaming mixins re-run `compute()` internally on every `update()`. This means streaming and batch modes produce **identical outputs by construction** — the streaming mode maintains a sliding window and calls the same math. No separate implementation to drift.

### Adding a new indicator:

1. Write C++ class with `compute()` method (batch)
2. Inherit appropriate streaming mixin
3. Add one line to `src/indicator/registry.def`
4. Auto-surfaces in Python/Node/Codon/QuickJS

---

<a id="q5"></a>
## Q5: Historical tick data availability from brokers

**Not all brokers provide tick-level historical data for 5-6 years.** Here's the landscape:

| Exchange/API | Tick History | Bar History | Notes |
|---|---|---|---|
| **Binance** | Last ~1 year via REST | Full history (2019+) via klines | Tick data via WebSocket recording |
| **Bybit** | Limited REST, WS recording | Good historical bars | V5 API |
| **Bitget** | Limited | Good bars | V2 API |
| **Hyperliquid** | Limited | Good recent bars | Newer exchange |
| **CoinAPI / Tardis.dev** | Full tick history (paid) | Full bar history | Third-party data providers |
| **CCXT** | Via exchange adapters | Via exchange adapters | Unified API |

### Practical approach for FLOX:

**Strategy 1: Use bar data (recommended for most cases)**

Most systematic strategies work on bar close. 1M bars from Binance are free and go back to 2019.

**Strategy 2: Record ticks going forward, use bars for history**

```bash
# Start recording tick data now
flox tape record --exchange binance --symbol BTCUSDT --output btcusdt.floxlog
```

This gives you a growing archive. For backtesting, use bars for pre-recording period and mix in tick data as it accumulates.

**Strategy 3: Buy historical ticks**

Tardis.dev provides full order book + trade history for major exchanges. Costs money but gives you the complete picture.

### Making FLOX native bars without ticks:

You don't need ticks to make bars. You can:

1. **Download 1M bars from Binance** → convert to MmapBarStorage
2. **Aggregate into any timeframe** using BarAggregator with custom BarPolicy
3. **Use `run_csv()` or `run_bars()` for backtesting**

The tick data requirement only matters if you need:
- Exact fill prices within a bar
- Order book depth simulation
- Volume profile / footprint chart analysis

---

<a id="q6"></a>
## Q6: Incremental data updates — deltas and merging

### Approach 1: Scheduled recording (recommended)

Run `flox tape record` on a schedule (cron/systemd) to capture new data:

```bash
# Record continuously
flox tape record --exchange binance --symbol BTCUSDT \
    --output /data/floxlog/BTCUSDT/$(date +%Y%m%d).floxlog

# Or in your strategy via MarketDataRecorder
```

FLOX's `.floxlog` format supports **segments** — each recording session creates a new segment. Segments can be concatenated:

```
BTCUSDT_2026_01.floxlog  (Segment 1: Jan trades)
BTCUSDT_2026_02.floxlog  (Segment 2: Feb trades)
BTCUSDT_2026_03.floxlog  (Segment 3: Mar trades)
```

`SegmentOps` provides merge, split, validate, and time-range extraction:

```cpp
#include <flox/replay/segment_ops.h>

// Merge segments into one file
flox::replay::SegmentOps::merge(
    {"jan.floxlog", "feb.floxlog", "mar.floxlog"},
    "q1_2026.floxlog"
);

// Extract time range
flox::replay::SegmentOps::extract_range(
    "q1_2026.floxlog",
    TimeRange{.start = "2026-02-01", .end = "2026-02-28"},
    "feb_only.floxlog"
);
```

### Approach 2: Live strategy that also records

You can have a "data collector" strategy that just records:

```cpp
class DataCollectorStrategy : public flox::Strategy {
    std::shared_ptr<flox::replay::MarketDataRecorder> recorder_;
public:
    void onSymbolTrade(flox::SymbolId sym, const flox::Trade& trade) override {
        recorder_->record_trade(trade);
    }
    void onSymbolBook(flox::SymbolId sym, const flox::BookUpdate& update) override {
        recorder_->record_book(update);
    }
};
```

Run this alongside your trading strategies.

### Approach 3: For bar data, just download new candles

```python
# Python script (cron daily)
import requests
import pandas as pd

def download_binance_klines(symbol, interval, start_ms, end_ms):
    url = f"https://fapi.binance.com/fapi/v1/klines"
    params = {"symbol": symbol, "interval": interval,
              "startTime": start_ms, "endTime": end_ms, "limit": 1500}
    resp = requests.get(url, params=params)
    # Parse and append to existing CSV/bar file
```

Run daily to append new 1M candles, then re-convert to MmapBarStorage.

### For pre-aggregated bars:

```cpp
// Append new bars to existing MmapBarStorage
auto storage = MmapBarStorage::open("bars_60s.bin", /*readonly=*/false);
storage.append(new_bars_begin, new_bars_end);
```

### Best practice:

1. **Bars**: Download incrementally from Binance API daily, append to MmapBarStorage
2. **Ticks**: Record continuously via `flox tape record`, rotate segments daily/monthly
3. **Merge periodically**: Run `SegmentOps::merge()` to consolidate segments for backtesting
4. **Validate**: Run `flox tape inspect --validate` after merging to check CRC32 integrity

---

<a id="q7"></a>
## Q7: Live trading data subscription, warmup, and Oryon comparison

### How live data subscription works in FLOX:

```
Exchange WebSocket
    │
    ▼
Connector (e.g., BinanceConnector)
    │
    ├─► TradeBus → Strategy::onSymbolTrade()
    ├─► BookUpdateBus → Strategy::onSymbolBook()
    │
    └─► BarAggregator → BarBus → Strategy::onSymbolBar()
         │
         └─► MultiTimeframeAggregator → Multiple BarBuses
```

The `Engine` class orchestrates everything:

```cpp
flox::EngineConfig config;
config.exchange = "binance";
config.symbols = {"BTCUSDT", "ETHUSDT"};

auto engine = flox::Engine(config);

// Register your strategy
engine.add_strategy(my_strategy);

// Register symbols
auto btc_id = engine.symbol_registry().register_symbol("binance", "BTCUSDT");
auto eth_id = engine.symbol_registry().register_symbol("binance", "ETHUSDT");

// Wire bar aggregation
engine.add_bar_aggregator<TimeBarPolicy>(btc_id, std::chrono::minutes(1));
engine.add_bar_aggregator<TimeBarPolicy>(btc_id, std::chrono::hours(4));

// Start — connects to exchange, subscribes to streams
engine.launch();
```

### Warmup — the critical question:

**Problem**: When a live strategy starts, indicators like SMA(200) need 200 bars of history. You can't wait 200 × 4H = 33 days.

#### Option 1: Warm up from historical data (recommended)

```cpp
// Load historical bars
auto hist_storage = MmapBarStorage::open("bars_4h.bin");

// Pre-populate BarMatrix with historical data
auto& bar_matrix = strategy.bar_matrix();
for (const auto& bar : hist_storage) {
    bar_matrix.update(btc_id, bar_4h_type, bar);
}

// Pre-populate IndicatorGraph
auto& graph = strategy.indicator_graph();
for (const auto& bar : hist_storage) {
    graph.update(bar);
}
// Now graph.ready() returns true for all indicators
```

#### Option 2: Warm up from live data only

```cpp
// Strategy checks readiness
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    if (!indicator_graph_.ready()) {
        return;  // Skip trading until warm
    }
    // ... trading logic
}
```

This is simple but means your strategy is blind for hours/days.

#### Option 3: Hybrid (best practice)

1. Warm up indicators from historical data (last 200-500 bars)
2. Continue updating from live data
3. IndicatorGraph seamlessly transitions — no special handling needed

```cpp
// In strategy init:
void onInit() override {
    // Load last 500 bars of history
    auto bars = load_recent_bars("BTCUSDT", "4H", count=500);
    for (auto& bar : bars) {
        indicator_graph_.update(bar);
        bar_matrix_.update(btc_id, bar_4h_type, bar);
    }
    LOG(info) << "Warmup complete, indicators ready: "
              << indicator_graph_.ready();
}
```

### Oryon vs FLOX warmup comparison:

| Aspect | FLOX (C++) | Oryon (Rust) |
|--------|-----------|--------------|
| **Warmup source** | Historical bars (MmapBarStorage) or live | Historical candles or live |
| **Indicator state** | StreamingIndicator maintains internal ring buffer | Pipeline stages maintain state |
| **Batch/streaming parity** | Same `compute()` code, guaranteed identical | `update()` and `run_research()` share code by construction |
| **Reset between folds** | `indicator.reset()` | `pipeline.reset()` |
| **Memory** | Pre-allocated ring buffers (zero allocation hot path) | Similar zero-allocation approach |
| **Speed** | Faster (no GC, template-based) | Fast (Rust, no GC) |

**Key difference**: Oryon's `run_research()` is batch mode on a `FeaturePipeline` — you pass a whole dataset and get all features at once. FLOX's equivalent is `compute()` (batch) on indicator arrays. Both systems guarantee batch and streaming produce identical results.

---

<a id="q8"></a>
## Q8: Oryon (Rust) integration with FLOX C++ strategies

### Direct integration is NOT practical:

Oryon is a separate Rust codebase. Calling Rust from C++ requires FFI bindings, and the indicator implementations are in Rust — you'd need to:
1. Compile Oryon as a static/dynamic library
2. Write C FFI wrappers for every indicator
3. Marshal data between C++ and Rust representations

This is technically possible but adds complexity with no clear benefit.

### How Oryon integrates with FLOX:

Oryon is designed as a **complementary** pipeline, not an in-process library:

```
Oryon (Rust)                          FLOX (C++)
─────────────                         ─────────
Historical data (Parquet)
    │
    ▼
FeaturePipeline
    │
    ├─► run_research() → features.parquet
    │                         │
    │                         ▼
    │                    Pre-computed features
    │                    (CSV/Parquet columns)
    │                         │
    │                         ▼
    │                    FLOX strategy reads
    │                    as additional bar data
    │
    └─► update() (live) ──► Shared memory / file ──► FLOX strategy reads
```

### Practical approaches:

#### 1. Pre-compute Oryon features, use in FLOX backtest

```python
# Step 1: Generate features with Oryon
oryon_features = pipeline.run_research(data)
oryon_features.to_csv("btcusdt_features.csv")

# Step 2: Load in FLOX as extra bar columns
# Modify CSV to add feature columns, or join on timestamp
```

#### 2. Run Oryon as a separate service for live trading

```
Oryon Service (Rust)          FLOX Strategy (C++)
┌──────────────────┐          ┌──────────────────┐
│ Subscribes to    │          │ Subscribes to    │
│ exchange WS      │          │ exchange WS      │
│                  │          │                  │
│ Computes features│──IPC──►  │ Reads features   │
│ (regime, etc.)   │  (shared │ + own indicators │
│                  │   memory │                  │
│                  │   or ZMQ)│ Makes trading    │
│                  │          │ decisions        │
└──────────────────┘          └──────────────────┘
```

#### 3. Rewrite Oryon indicators in FLOX C++ (recommended for deep integration)

Since FLOX's C++ indicators auto-surface in all bindings, and the math is the same:

```cpp
// Reimplement Oryon's regime detection as a FLOX indicator
class RegimeIndicator : public flox::indicator::StreamingBar<double> {
    // Same math as Oryon's Rust implementation
    double compute(const std::vector<Bar>& bars) override;
    void update(const Bar& bar) override;
};
```

### Recommendation:

- **Don't mix FFI**: If you're using FLOX C++ as primary, reimplement needed Oryon indicators in C++ FLOX style
- **Use Oryon for research**: Oryon's `run_research()` batch mode is great for feature engineering and exploration
- **Use FLOX for production**: C++ indicators in FLOX are faster, type-safe, and auto-bind to Python

---

<a id="q9"></a>
## Q9: Feeding data to backtesting — formats and Indicator Graph

### Data formats for backtesting:

| Format | Method | Speed | Use Case |
|--------|--------|-------|----------|
| CSV | `run_csv()` | Slow | Quick prototyping, broker data |
| MmapBarStorage | `run_bars()` | Fast | Production backtests |
| `.floxlog` | `run()` with ReplayConnector | Medium | Tick-level fidelity |

### CSV backtest:

```cpp
BacktestRunner runner;
auto result = runner.run_csv("btcusdt_4h.csv", strategy, executor);
```

Expected CSV format:
```
timestamp,open,high,low,close,volume
2020-01-01T00:00:00,7200.5,7215.3,7198.1,7210.2,1234.56
```

### Pre-aggregated bar backtest:

```cpp
auto storage = MmapBarStorage::open("bars_4h.bin");
BacktestRunner runner;
auto result = runner.run_bars(*storage, strategy, executor);
```

### Using Indicator Graph in backtesting:

The Indicator Graph works identically in backtesting and live:

```cpp
class MyStrategy : public flox::Strategy {
    flox::indicator::IndicatorGraph graph_;
    
    void onInit() override {
        // Set up indicator graph once
        graph_.add_node("sma_fast", [](auto&) {
            return std::make_shared<flox::indicator::StreamingSMA>(20);
        });
        graph_.add_node("sma_slow", [](auto&) {
            return std::make_shared<flox::indicator::StreamingSMA>(50);
        });
    }
    
    void onSymbolBar(SymbolId sym, const Bar& bar) override {
        graph_.update(bar);  // Updates all indicators
        
        if (!graph_.ready()) return;
        
        double fast = graph_.require("sma_fast")->value();
        double slow = graph_.require("sma_slow")->value();
        
        // Trading logic...
    }
};
```

**In backtesting**, `onSymbolBar()` is called for each historical bar in sequence. The Indicator Graph accumulates state just like in live trading.

**No special handling needed** — the same strategy code works in both modes.

### Pre-calculated features:

If you have pre-calculated indicator values (e.g., from Oryon), you can:

1. **Add as extra CSV columns**: `timestamp,open,high,low,close,volume,my_feature`
2. **Read in strategy**: Access via bar metadata or a side-channel
3. **Use Indicator Graph for derived calculations**: Feed pre-calculated values as inputs to higher-level indicators

```cpp
// Custom indicator that uses pre-calculated features
class PreCalcFeature : public flox::indicator::StreamingSingle<double> {
    std::vector<double> values_;
    size_t idx_ = 0;
public:
    PreCalcFeature(std::vector<double> values) : values_(std::move(values)) {}
    void update(double) override { /* no-op, reads from pre-calc */ }
    double value() const override { return values_[idx_]; }
    bool ready() const override { return idx_ >= warmup_; }
};
```

---

<a id="q10"></a>
## Q10: Live trading — metrics storage, memory, warmup best practices

### Where metrics are stored:

FLOX uses **in-memory** storage for all live trading metrics:

```
PositionTracker ──► Current positions (in memory)
OrderTracker ──► Open order state (in memory)
PnLTracker ──► Realized/unrealized PnL (in memory)
BarMatrix ──► Recent bars (ring buffer, default 64 per symbol per TF)
IndicatorGraph ──► Indicator state (in memory)
```

**Persistence** is your responsibility:
- Write trade logs via `flox tape record` or custom sink
- Use `AbstractStorageSink` to persist positions/orders periodically
- Write equity curve to CSV/DB periodically

### Memory usage:

| Component | Memory | Notes |
|-----------|--------|-------|
| BarMatrix | ~64 bars × 8 doubles × N_symbols × N_TFs | Configurable depth (default 64) |
| IndicatorGraph | Depends on indicators | SMA(N) stores N values, RSI stores ~1 value |
| OrderBook (L2) | N_levels × 16 bytes | NlevelOrderBook, configurable depth |
| PositionTracker | O(N_open_positions) | Minimal |
| TradeBus/BookUpdateBus | Ring buffer size × event size | Pre-allocated |

**Typical single-symbol setup**: 10-50 MB total.
**Multi-symbol (20 pairs, 4 TFs)**: 100-500 MB.

Memory is pre-allocated at startup via:
- **PMR (Polymorphic Memory Resources)**: std::pmr for containers
- **Object pools**: Pre-allocated Handle<T> pools
- **Ring buffers**: Fixed-size, no allocation after init
- **MmapBarStorage**: OS-managed, doesn't count against process memory

### Warmup best practices:

**DO: Warm up from historical data**

```cpp
void onInit() override {
    // 1. Load last N bars from MmapBarStorage
    auto storage = MmapBarStorage::open("bars_4h.bin");
    auto recent = storage.last(500);  // Last 500 bars
    
    // 2. Feed through indicator graph
    for (const auto& bar : recent) {
        indicator_graph_.update(bar);
        bar_matrix_.update(symbol_id_, bar_4h_type, bar);
    }
    
    // 3. Verify readiness
    if (!indicator_graph_.ready()) {
        LOG(warn) << "Indicators not fully warmed up";
    }
}
```

**DON'T: Wait for live warmup**

This wastes hours/days of live data where your strategy can't trade.

**DON'T: Mix historical and live data without care**

Ensure historical data ends where live data begins. No gaps, no overlaps.

```cpp
// Get last historical bar timestamp
auto last_hist_ts = storage.last().endTime;

// Only start consuming live bars AFTER this timestamp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    if (bar.startTime <= last_hist_ts_) return;  // Skip overlap
    // ... normal logic
}
```

### Monitoring live metrics:

```cpp
// Access live metrics from strategy
double unrealized_pnl = position_tracker_.unrealized_pnl(symbol_id);
int open_positions = position_tracker_.open_count();
double equity = position_tracker_.total_equity();
double drawdown = position_tracker_.max_drawdown();
```

---

<a id="q11"></a>
## Q11: Trading entities explained — for trading newbies

### The Market Ecosystem

#### Participants

| Participant | Role | Example |
|-------------|------|---------|
| **Retail traders** | Small accounts, emotional decisions | You, me, Reddit traders |
| **Market makers** | Provide liquidity, profit from spread | Jump Trading, Wintermute |
| **Institutional** | Large orders, move markets | Hedge funds, banks |
| **Arbitrageurs** | Exploit price differences across venues | Cross-exchange bots |
| **Exchanges** | Match buyers and sellers, charge fees | Binance, Bybit |

### Core Entities and Relationships

```
┌─────────────────────────────────────────────────────┐
│                    EXCHANGE                          │
│                                                     │
│  ┌─────────────────────────────────────────────┐    │
│  │              ORDER BOOK (L2)                 │    │
│  │                                              │    │
│  │  BIDS (buy orders)    ASKS (sell orders)     │    │
│  │  Price  Qty           Price  Qty             │    │
│  │  50100  1.5           50101  2.0             │    │
│  │  50099  3.0           50102  1.5             │    │
│  │  50098  2.0           50103  4.0             │    │
│  │  ...                  ...                    │    │
│  └─────────────────────────────────────────────┘    │
│         │                           │                │
│         ▼                           ▼                │
│  ┌──────────────────────────────────────────┐       │
│  │              TRADES                       │       │
│  │  When a buy order matches a sell order    │       │
│  │  → Trade happens at execution price       │       │
│  └──────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────┘
```

### Entity Definitions

#### 1. ORDER BOOK

The order book is a real-time list of all outstanding buy and sell orders, organized by price level.

- **Bid**: The highest price a buyer is willing to pay. The best bid is called **BBO** (Best Bid Offer).
- **Ask**: The lowest price a seller is willing to accept. Also called the **offer**.
- **Spread**: `Ask - Bid`. The gap between best bid and best ask. This is the market maker's profit.
- **Mid price**: `(Best Bid + Best Ask) / 2`. The "fair" price.
- **Depth**: Number of price levels visible. L2 = aggregated per price, L3 = individual orders.

**In FLOX**: `NlevelOrderBook` maintains a sorted list of price levels. Updated via `BookUpdate` events.

```cpp
auto book = engine.order_book(btc_id);
double best_bid = book.best_bid();
double best_ask = book.best_ask();
double spread = best_ask - best_bid;
double mid = (best_bid + best_ask) / 2.0;
```

#### 2. TRADE

A trade occurs when a buyer's bid matches a seller's ask. This is the actual transaction.

- **Price**: The execution price
- **Quantity**: Amount traded
- **Side**: Was the aggressor buying (ask was hit) or selling (bid was hit)?
- **Timestamp**: When it happened

**In FLOX**: `Trade` struct with `price`, `quantity`, `side`, `timestamp`.

```cpp
void onSymbolTrade(SymbolId sym, const Trade& trade) override {
    LOG(info) << "Trade: " << trade.quantity << " @ " << trade.price
              << " side=" << (trade.is_buy ? "BUY" : "SELL");
}
```

#### 3. ORDER

An instruction to buy or sell. Types:

| Order Type | Description | FLOX Method |
|------------|-------------|-------------|
| **Market** | Execute immediately at best available price | `emitMarketBuy(qty)` |
| **Limit** | Execute only at specified price or better | `emitLimitBuy(price, qty)` |
| **Stop Market** | Market order triggered when price hits stop | `emitStopMarket(side, stop_price, qty)` |
| **Stop Limit** | Limit order triggered at stop price | `emitStopLimit(side, stop_price, limit_price, qty)` |
| **Trailing Stop** | Stop that moves with price | `emitTrailingStop(side, trail_amount, qty)` |
| **Take Profit** | Close position at profit target | `emitTakeProfitMarket(side, tp_price, qty)` |

**Order lifecycle in FLOX**:

```
emitMarketBuy()
    │
    ▼
Order (state=NEW)
    │
    ▼
Executor::submit(order)
    │
    ▼
OrderEvent (ACCEPTED)
    │
    ▼
OrderEvent (FILLED) or (REJECTED) or (CANCELED)
    │
    ▼
Fill record → PositionTracker updates position
```

#### 4. POSITION

A position is your current market exposure.

- **Long**: You bought, profit if price goes up
- **Short**: You sold, profit if price goes down
- **Size**: How much you hold
- **Entry price**: Average price of your position
- **Unrealized PnL**: Current profit/loss if you closed now
- **Realized PnL**: Profit/loss from closed trades

**In FLOX**: `PositionTracker` manages positions.

```cpp
// Check current position
auto pos = position_tracker_.get_position(btc_id);
if (pos) {
    LOG(info) << "Size: " << pos->size
              << " Entry: " << pos->entry_price
              << " Unrealized PnL: " << pos->unrealized_pnl;
}
```

#### 5. SLIPPAGE

The difference between expected price and actual fill price.

- You send a market buy at $50,000
- By the time it fills, price moved to $50,002
- Slippage = $2 (or 0.04%)

**Causes**: Network latency, order queue position, market impact.

**In FLOX**: Configurable slippage models for backtesting:
- `NONE` — no slippage
- `FIXED_TICKS` — constant slippage
- `FIXED_BPS` — percentage-based
- `VOLUME_IMPACT` — proportional to order size vs. bar volume

#### 6. IMBALANCE

Order flow imbalance: more buying pressure vs. selling pressure at a price level.

- **Bid imbalance**: Large buy orders vs. sell orders → price likely to go up
- **Ask imbalance**: Large sell orders vs. buy orders → price likely to go down

**In FLOX**: `FootprintChart` tracks bid/ask volume per price level.

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    auto footprint = bar.footprint();
    for (auto& level : footprint.levels()) {
        double imbalance = level.ask_volume - level.bid_volume;
        // Positive = selling pressure, Negative = buying pressure
    }
}
```

#### 7. L1 / L2 / L3 DATA

| Level | Content | Typical Use |
|-------|---------|-------------|
| **L1** (Top of Book) | Best bid, best ask, last trade price | Quick price checks |
| **L2** (Market Depth) | Aggregated orders per price level | Most strategies |
| **L3** (Full Depth) | Individual orders with IDs | HFT, market making |

**In FLOX**: `NlevelOrderBook` for L2, `L3OrderBook` for L3.

### Why is it possible to earn on trading?

#### Edge sources:

1. **Information edge**: Processing data faster or better than others
2. **Behavioral edge**: Exploiting systematic human biases (panic selling, FOMO buying)
3. **Structural edge**: Market microstructure (funding rates, liquidations)
4. **Statistical edge**: Mean reversion, momentum, regime detection

#### Price inefficiency:

Prices are "efficient" when they reflect all available information. Inefficiencies exist because:
- Information arrives at different times for different participants
- Some participants trade emotionally, not rationally
- Market structure creates friction (fees, latency, position limits)
- Large orders move prices temporarily

**Your strategy's job**: Detect these inefficiencies and exploit them before they disappear.

### How it all flows in FLOX:

```
Exchange WebSocket
    │
    ├─► Trade events ──► Strategy::onSymbolTrade()
    ├─► Book updates ──► Strategy::onSymbolBook()  (L2/L3)
    │
    └─► BarAggregator
         │
         ├─► Bar events ──► Strategy::onSymbolBar()
         │                     │
         │                     ├─► IndicatorGraph.update()
         │                     ├─► Check signals
         │                     └─► Emit orders via executor
         │
         └─► MultiTimeframeAggregator
              ├─► 1M bars
              ├─► 5M bars
              ├─► 1H bars
              └─► 4H bars
    
Orders emitted by strategy:
    │
    ▼
SimulatedExecutor (backtest) or LiveExecutor (production)
    │
    ▼
OrderEvents (ACCEPTED, FILLED, REJECTED, CANCELED)
    │
    ▼
PositionTracker updates
    │
    ▼
RiskManager checks (drawdown, exposure limits)
    │
    ▼
KillSwitch (emergency halt if risk limits breached)
```

### FLOX event dispatching explained:

`EventDispatcher<T>` is a compile-time dispatch mechanism. It routes events to subscriber methods **without virtual functions or dynamic casting**. This is why FLOX is fast.

```cpp
// Internal dispatch (simplified)
template<>
void EventDispatcher<TradeEvent>::dispatch(const TradeEvent& event) {
    // Calls onSymbolTrade on all registered strategies
    for (auto& subscriber : subscribers_) {
        subscriber.onSymbolTrade(event.symbol_id, event.trade);
    }
}

template<>
void EventDispatcher<BarEvent>::dispatch(const BarEvent& event) {
    // Calls onSymbolBar on all registered strategies
    for (auto& subscriber : subscribers_) {
        subscriber.onSymbolBar(event.symbol_id, event.bar);
    }
}
```

**OrderEvent** is different — it's dispatched via `OrderExecutionBus`:

```cpp
// Order lifecycle events
OrderExecutionBus::dispatch(OrderEvent{
    .order_id = 12345,
    .type = OrderEventType::FILLED,
    .fill_price = 50100.0,
    .fill_qty = 0.5
});

// Heard by:
// - OrderTracker (updates order state)
// - PositionTracker (updates position)
// - PnLTracker (calculates PnL)
// - Your strategy (via AbstractExecutionListener)
```

The `AbstractExecutionListener` interface lets your strategy react to its own order events:

```cpp
class MyStrategy : public flox::Strategy, public flox::AbstractExecutionListener {
    void onOrderAccepted(const Order& order) override {
        LOG(info) << "Order accepted: " << order.id;
    }
    void onOrderFilled(const Order& order, double fill_price, double fill_qty) override {
        LOG(info) << "Filled " << fill_qty << " @ " << fill_price;
    }
    void onOrderRejected(const Order& order, const std::string& reason) override {
        LOG(error) << "Order rejected: " << reason;
    }
};
```

---

<a id="q11b"></a>
## Q11b: Accessing previous bars in FLOX strategies

### You do NOT need to store bars manually. FLOX provides three mechanisms:

#### 1. BarMatrix (recommended for MTF)

```cpp
class MyStrategy : public flox::Strategy {
    void onInit() override {
        // BarMatrix is automatically available via base class
        // Configured with: <Symbols, Timeframes, Depth>
    }
    
    void onSymbolBar(SymbolId sym, const Bar& bar) override {
        // Current bar
        auto current = bar_matrix_.bar(sym, BarType::Time4H, /*lag=*/0);
        
        // Previous bar
        auto prev = bar_matrix_.bar(sym, BarType::Time4H, /*lag=*/1);
        
        // 10 bars ago
        auto old = bar_matrix_.bar(sym, BarType::Time4H, /*lag=*/10);
        
        // Last N bars as array
        auto recent = bar_matrix_.last_n(sym, BarType::Time4H, /*count=*/20);
    }
};
```

Default ring depth is 64 bars per (symbol, timeframe). Configurable.

#### 2. Strategy helper methods

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    // Last closed bar (previous bar, already complete)
    auto last_closed = last_closed_bar(sym, BarType::Time4H, /*param=*/0);
    
    // Last N closed bars
    auto last_20 = last_n_closed_bars(sym, BarType::Time4H, /*count=*/20);
    
    // These are bars that have ALREADY CLOSED — no partial bars
}
```

#### 3. IndicatorGraph (for indicator values, not raw bars)

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    
    // Indicator values already incorporate historical bars
    double sma20 = graph_.require("sma_20")->value();
    // This SMA already looked at the last 20 bars
}
```

### Historical data for warmup:

```cpp
void onInit() override {
    // Load historical bars to populate BarMatrix
    auto storage = MmapBarStorage::open("bars_4h.bin");
    
    // Feed last 200 bars into BarMatrix
    auto recent = storage.last(200);
    for (const auto& bar : recent) {
        bar_matrix_.update(symbol_id_, BarType::Time4H, bar);
        graph_.update(bar);  // Also warm up indicators
    }
}
```

### For multi-timeframe access:

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    // Access different timeframes simultaneously
    auto m1 = bar_matrix_.bar(sym, BarType::Time1M, 0);
    auto h1 = bar_matrix_.bar(sym, BarType::Time1H, 0);
    auto h4 = bar_matrix_.bar(sym, BarType::Time4H, 0);
    auto d1 = bar_matrix_.bar(sym, BarType::Time1D, 0);
    
    // Compare across timeframes
    if (h4.close > d1.close && m1.close > h1.close) {
        // Multi-TF alignment
    }
}
```

---

<a id="q12"></a>
## Q12: Strategy behavior — on_bar vs intrabar, batch vs streaming

### The two decision points:

#### Decision on bar close (`onSymbolBar`)

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    // Called once per bar when it closes
    // Bar is COMPLETE — OHLCV are final
    // Indicators are stable
    
    if (should_enter()) {
        emitMarketBuy(quantity);
    }
}
```

**Pros**: Simple, no noise, matches most backtests, sufficient for most systematic strategies.
**Cons**: Misses intrabar opportunities, can't react to sudden moves within a bar.

#### Decision on intrabar events (`onSymbolTrade`, `onSymbolBook`)

```cpp
void onSymbolTrade(SymbolId sym, const Trade& trade) override {
    // Called on EVERY trade
    // Can react to micro-moves, stop runs, etc.
    
    auto book = order_book(sym);
    double imbalance = book.bid_volume(0) - book.ask_volume(0);
    
    if (imbalance > threshold && has_position_) {
        // Close position early on order flow shift
        emitMarketSell(position_size_);
    }
}
```

**Pros**: Faster reaction, can catch intrabar opportunities, better stop management.
**Cons**: More noise, higher computational load, harder to backtest accurately.

### Recommended approach: Combine both

```cpp
class SmartStrategy : public flox::Strategy {
    bool signal_active_ = false;
    double stop_price_ = 0;
    double tp_price_ = 0;
    
    // === SIGNAL GENERATION: on bar close ===
    void onSymbolBar(SymbolId sym, const Bar& bar) override {
        graph_.update(bar);
        if (!graph_.ready()) return;
        
        // Generate signals on complete bars only
        double sma_fast = graph_.require("sma_fast")->value();
        double sma_slow = graph_.require("sma_slow")->value();
        
        if (sma_fast > sma_slow && !has_long_position()) {
            double atr = graph_.require("atr_14")->value();
            double qty = calculate_position_size(atr);
            
            // Calculate stop and TP levels
            stop_price_ = bar.close - 2.0 * atr;
            tp_price_ = bar.close + 3.0 * atr;
            
            // Entry on next bar open (or market now)
            emitMarketBuy(qty);
            signal_active_ = true;
        }
    }
    
    // === RISK MANAGEMENT: intrabar ===
    void onSymbolTrade(SymbolId sym, const Trade& trade) override {
        if (!signal_active_) return;
        
        // Check stop hit on every trade (not just bar close)
        if (trade.price <= stop_price_ && has_long_position()) {
            emitMarketSell(position_size_);
            signal_active_ = false;
        }
        
        // Check TP hit
        if (trade.price >= tp_price_ && has_long_position()) {
            emitMarketSell(position_size_);
            signal_active_ = false;
        }
    }
};
```

Or use the SimulatedExecutor's built-in stop/TP support:

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    if (should_enter()) {
        double atr = graph_.require("atr_14")->value();
        double entry = bar.close;
        
        // Submit entry + stop + TP as a package
        emitMarketBuy(qty);
        emitStopMarket(Side::SELL, entry - 2.0 * atr, qty);
        emitTakeProfitMarket(Side::SELL, entry + 3.0 * atr, qty);
    }
}
```

The SimulatedExecutor tracks conditional orders and triggers them on price movement — even within a bar.

### Batch vs Streaming in FLOX (paralleling Oryon):

**Oryon**: `update()` for streaming, `run_research()` for batch. Same code, identical outputs. `reset()` between folds.

**FLOX**: Same pattern, different names:

| Mode | Oryon (Rust) | FLOX (C++) | FLOX (Python) |
|------|-------------|-----------|---------------|
| **Batch** | `run_research(data)` | `compute(bars)` | `indicator.compute(data)` |
| **Streaming** | `update(tick)` | `update(bar)` then `value()` | `ind.update(price)` |
| **Ready check** | Always ready after init | `ready()` | `ind.ready` |
| **Reset** | `pipeline.reset()` | `indicator.reset()` | `ind.reset()` |
| **Value** | Return from `update()` | `value()` | `ind.value` |

In FLOX, the streaming mixin internally calls `compute()` on every update, guaranteeing parity:

```cpp
// StreamingSMA internally maintains a ring of last N values
// On each update(), it re-runs the SMA formula on the ring
void update(double price) override {
    ring_.push(price);
    if (ring_.size() >= period_) {
        value_ = std::accumulate(ring_.begin(), ring_.end(), 0.0) / period_;
        ready_ = true;
    }
}
```

---

<a id="q13"></a>
## Q13: Complex exit strategies — ATR stops, Chandelier, trailing, and exit alphas

This is the most complex part. FLOX doesn't have built-in "exit mode" enums like the Trader7-Kilo pipeline. Instead, you implement exit logic in your strategy using FLOX's building blocks.

### Available FLOX primitives for exits:

| Primitive | FLOX Method | Description |
|-----------|-----------|-------------|
| Market close | `emitMarketSell(qty)` | Immediate close |
| Stop market | `emitStopMarket(Side, stop_price, qty)` | Market order when price hits stop |
| Take profit market | `emitTakeProfitMarket(Side, tp_price, qty)` | Market order at profit target |
| Trailing stop (absolute) | `emitTrailingStop(Side, trail_amount, qty)` | Trails by fixed distance |
| Trailing stop (percent) | `emitTrailingStopPercent(Side, trail_pct, qty)` | Trails by percentage |
| Limit order | `emitLimitSell(price, qty)` | Close at limit price |

### Exit Mode Implementations

#### 1. signal — Exit on opposite signal only

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    if (!graph_.ready()) return;
    
    Signal sig = generate_signal();
    
    if (sig == Signal::LONG && !has_long_position()) {
        emitMarketBuy(qty_);
    } else if (sig == Signal::SHORT && has_long_position()) {
        emitMarketSell(position_size());
    }
}
```

#### 2. bracket — Fixed ATR stop-loss and take-profit

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    if (!graph_.ready()) return;
    
    if (should_enter_long() && !has_long_position()) {
        double atr = graph_.require("atr_14")->value();
        double entry = bar.close;
        double qty = calculate_qty();
        
        emitMarketBuy(qty);
        // Hard stop at entry - sl_atr_mult * ATR
        emitStopMarket(Side::SELL, entry - sl_atr_mult_ * atr, qty);
        // Take profit at entry + tp_atr_mult * ATR
        emitTakeProfitMarket(Side::SELL, entry + tp_atr_mult_ * atr, qty);
    }
}
```

#### 3. trail_atr — ATR trailing stop without Chandelier

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    if (!graph_.ready()) return;
    
    if (should_enter_long() && !has_long_position()) {
        double atr = graph_.require("atr_14")->value();
        emitMarketBuy(qty_);
        emitTrailingStop(Side::SELL, trail_atr_mult_ * atr, qty_);
    }
}
```

#### 4. chandelier — Chandelier trailing exit

Chandelier tracks the highest favorable price since entry and trails a stop below it:

```cpp
class ChandelierStrategy : public flox::Strategy {
    double highest_since_entry_ = 0;
    double lowest_since_entry_ = std::numeric_limits<double>::max();
    bool in_long_ = false;
    double entry_price_ = 0;
    
    void onSymbolBar(SymbolId sym, const Bar& bar) override {
        graph_.update(bar);
        if (!graph_.ready()) return;
        
        double atr = graph_.require("atr_14")->value();
        
        // Track extremes since entry
        if (in_long_) {
            highest_since_entry_ = std::max(highest_since_entry_, bar.high);
            
            // Chandelier stop = highest - lookback_high_to_low_range + atr_mult * ATR
            // Simplified: trail from highest high
            double chandelier_stop = highest_since_entry_ - chandelier_mult_ * atr;
            
            if (bar.close <= chandelier_stop) {
                emitMarketSell(position_tracker_.position_size(sym));
                in_long_ = false;
            }
        }
        
        if (should_enter_long() && !in_long_) {
            emitMarketBuy(calculate_qty());
            in_long_ = true;
            entry_price_ = bar.close;
            highest_since_entry_ = bar.high;
        }
    }
};
```

#### 5. chandelier_ema — Chandelier + EMA recross exit

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    if (!graph_.ready()) return;
    
    double atr = graph_.require("atr_14")->value();
    double ema = graph_.require("ema_trend")->value();
    
    if (in_long_) {
        highest_since_entry_ = std::max(highest_since_entry_, bar.high);
        double chandelier_stop = highest_since_entry_ - chandelier_mult_ * atr;
        
        // Exit condition 1: Chandelier stop hit
        bool stop_hit = bar.close <= chandelier_stop;
        
        // Exit condition 2: Price recrosses below EMA (failed breakout)
        bool ema_recross = bar.close < ema && prev_close_ >= ema;
        
        if (stop_hit || ema_recross) {
            emitMarketSell(position_size());
            in_long_ = false;
        }
    }
    
    // Entry logic...
    prev_close_ = bar.close;
}
```

#### 6. chandelier_full — Chandelier + EMA + time stop

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    if (!graph_.ready()) return;
    
    double atr = graph_.require("atr_14")->value();
    double ema = graph_.require("ema_trend")->value();
    
    if (in_long_) {
        highest_since_entry_ = std::max(highest_since_entry_, bar.high);
        bars_since_entry_++;
        
        double chandelier_stop = highest_since_entry_ - chandelier_mult_ * atr;
        
        bool stop_hit = bar.close <= chandelier_stop;
        bool ema_recross = bar.close < ema && prev_close_ >= ema;
        bool time_stop = bars_since_entry_ >= max_holding_bars_;
        
        if (stop_hit || ema_recross || time_stop) {
            emitMarketSell(position_size());
            in_long_ = false;
        }
    }
    // Entry...
}
```

#### 7. signal_be_trail — Signal + breakeven + trailing

```cpp
class SignalBETrailStrategy : public flox::Strategy {
    double entry_price_ = 0;
    double trail_stop_ = 0;
    bool be_activated_ = false;
    double be_trigger_r_ = 1.0;  // Activate BE at 1R profit
    double trail_atr_mult_ = 2.5;
    
    void onSymbolBar(SymbolId sym, const Bar& bar) override {
        graph_.update(bar);
        if (!graph_.ready()) return;
        
        double atr = graph_.require("atr_14")->value();
        Signal sig = generate_signal();
        
        if (has_long_position()) {
            double unrealized = bar.close - entry_price_;
            double initial_risk = sl_atr_mult_ * atr;
            
            // Breakeven trigger: price moved 1R in our favor
            if (!be_activated_ && unrealized >= be_trigger_r_ * initial_risk) {
                be_activated_ = true;
                trail_stop_ = entry_price_;  // Move stop to breakeven
            }
            
            // Trailing: once BE activated, trail by ATR
            if (be_activated_) {
                double new_trail = bar.close - trail_atr_mult_ * atr;
                trail_stop_ = std::max(trail_stop_, new_trail);
            }
            
            // Check exits:
            // 1. Trailing/BE stop hit
            if (bar.close <= trail_stop_) {
                emitMarketSell(position_size());
                reset_state();
                return;
            }
            
            // 2. Signal reversal
            if (sig == Signal::SHORT) {
                emitMarketSell(position_size());
                reset_state();
                return;
            }
        }
        
        // Entry on signal
        if (sig == Signal::LONG && !has_long_position()) {
            double qty = calculate_qty();
            emitMarketBuy(qty);
            entry_price_ = bar.close;
            trail_stop_ = bar.close - sl_atr_mult_ * atr;  // Initial hard stop
            be_activated_ = false;
        }
    }
    
    void reset_state() {
        entry_price_ = 0;
        trail_stop_ = 0;
        be_activated_ = false;
    }
};
```

#### 8. bar_close_exact_sl_tp — Conservative next-bar execution

```cpp
void onSymbolBar(SymbolId sym, const Bar& bar) override {
    graph_.update(bar);
    if (!graph_.ready()) return;
    
    // Check SL/TP based on CLOSED bar, execute on NEXT bar open
    // This avoids intrabar fills
    
    if (has_long_position()) {
        double atr = graph_.require("atr_14")->value();
        double sl = entry_price_ - sl_atr_mult_ * atr;
        double tp = entry_price_ + tp_atr_mult_ * atr;
        
        // Evaluate on bar close, not intrabar
        if (bar.close <= sl || bar.close >= tp) {
            // Will execute at next bar's open
            emitMarketSell(position_size());
        }
    }
    // Entry...
}
```

### Exit Alphas Implementation

Each exit alpha is a condition that can trigger an exit:

```cpp
class AlphaAwareStrategy : public flox::Strategy {
    struct ExitState {
        double entry_price = 0;
        double highest_favorable = 0;
        int bars_since_entry = 0;
        double total_funding_paid = 0;
    } state_;
    
    void onSymbolBar(SymbolId sym, const Bar& bar) override {
        graph_.update(bar);
        if (!graph_.ready() || !has_long_position()) return;
        
        double atr = graph_.require("atr_14")->value();
        state_.bars_since_entry++;
        state_.highest_favorable = std::max(state_.highest_favorable, bar.high);
        
        // --- Alpha 1: ATR-based stop ---
        double atr_stop = state_.entry_price - sl_atr_mult_ * atr;
        if (bar.close <= atr_stop) { close_position(); return; }
        
        // --- Alpha 2: Chandelier trail ---
        double chandelier = state_.highest_favorable - chandelier_mult_ * atr;
        if (bar.close <= chandelier) { close_position(); return; }
        
        // --- Alpha 3: Breakeven activation ---
        double r_multiple = (bar.close - state_.entry_price) / (sl_atr_mult_ * atr);
        if (r_multiple >= be_trigger_r_ && !be_active_) {
            be_active_ = true;
            trail_level_ = state_.entry_price;
        }
        
        // --- Alpha 4: Time stop ---
        if (state_.bars_since_entry >= max_holding_bars_) {
            close_position(); return;
        }
        
        // --- Alpha 5: Signal reversal ---
        Signal sig = generate_signal();
        if (sig == Signal::SHORT) { close_position(); return; }
        
        // --- Alpha 6: Regime invalidation ---
        double regime = graph_.require("regime")->value();
        if (regime != favorable_regime_) { close_position(); return; }
        
        // --- Alpha 7: Volatility expansion ---
        double current_atr = atr;
        double avg_atr = graph_.require("atr_sma")->value();
        if (current_atr > vol_expansion_mult_ * avg_atr) {
            close_position(); return;  // Exit on vol spike
        }
        
        // --- Alpha 8: Funding cost accumulation ---
        // Track in onSymbolTrade or from exchange data
        state_.total_funding_paid += get_funding_cost(sym);
        if (state_.total_funding_paid >= max_funding_cost_) {
            close_position(); return;
        }
        
        // --- Alpha 9: Adverse CVD shift ---
        double cvd = graph_.require("cvd")->value();
        double cvd_sma = graph_.require("cvd_sma")->value();
        if (in_long_ && cvd < cvd_sma * cvd_threshold_) {
            close_position(); return;
        }
        
        // --- Alpha 10: VPOC reach ---
        auto profile = volume_profile_.get_vpoc(sym);
        if (std::abs(bar.close - profile) <= vpoc_proximity_) {
            partial_close(0.5);  // Take partial profit
        }
        
        // --- Alpha 11: Structural level ---
        for (double level : structural_levels_) {
            if (std::abs(bar.close - level) <= level_proximity_) {
                partial_close(0.3);
            }
        }
    }
};
```

### Grid search over exit parameters:

```cpp
struct ExitParams {
    double trail_atr_mult;
    double sl_atr_mult;
    int chandelier_lookback;
    double tp_atr_mult;
    int atr_period;
    
    std::string toString() const {
        return fmt::format("trail={:.1f}_sl={:.1f}_chan={}_tp={:.1f}_atr={}",
            trail_atr_mult, sl_atr_mult, chandelier_lookback,
            tp_atr_mult, atr_period);
    }
};

struct ExitGrid {
    std::vector<double> trail_mults = {2.0, 2.5, 3.0, 3.5, 4.0};
    std::vector<double> sl_mults = {1.0, 1.5, 2.0, 2.5, 3.0};
    std::vector<int> chan_lookbacks = {14, 22, 34, 55};
    std::vector<double> tp_mults = {1.0, 1.5, 2.0, 3.0};
    std::vector<int> atr_periods = {10, 14, 21};
    
    size_t totalCombinations() const {
        return trail_mults.size() * sl_mults.size() * chan_lookbacks.size()
             * tp_mults.size() * atr_periods.size();
    }
    
    ExitParams operator[](size_t idx) const {
        // Flatten multi-dimensional index
        size_t i = idx;
        size_t a = i % atr_periods.size(); i /= atr_periods.size();
        size_t t = i % tp_mults.size(); i /= tp_mults.size();
        size_t c = i % chan_lookbacks.size(); i /= chan_lookbacks.size();
        size_t s = i % sl_mults.size(); i /= sl_mults.size();
        size_t tr = i % trail_mults.size();
        
        return ExitParams{
            trail_mults[tr], sl_mults[s], chan_lookbacks[c],
            tp_mults[t], atr_periods[a]
        };
    }
};

// Run grid search
auto optimizer = BacktestOptimizer<ExitParams, ExitGrid>();
auto results = optimizer.runLocal(grid, storage, strategy_factory);
auto ranked = optimizer.rankResults(results, "sharpe");
optimizer.exportToCSV(ranked, "exit_grid_results.csv");
```

---

## Quick Reference: FLOX Strategy Template

```cpp
#pragma once
#include <flox/strategy/strategy.h>
#include <flox/indicator/indicator_graph.h>
#include <flox/indicator/sma.h>
#include <flox/indicator/atr.h>
#include <flox/indicator/rsi.h>

class MyStrategy : public flox::Strategy {
    flox::indicator::IndicatorGraph graph_;
    double qty_ = 0.01;
    
public:
    void onInit() override {
        // Wire up indicators
        graph_.add_node("sma_fast", [](auto&) {
            return std::make_shared<flox::indicator::StreamingSMA>(20);
        });
        graph_.add_node("sma_slow", [](auto&) {
            return std::make_shared<flox::indicator::StreamingSMA>(50);
        });
        graph_.add_node("atr", [](auto&) {
            return std::make_shared<flox::indicator::StreamingATR>(14);
        });
    }
    
    void onSymbolBar(flox::SymbolId sym, const flox::Bar& bar) override {
        graph_.update(bar);
        if (!graph_.ready()) return;
        
        double fast = graph_.require("sma_fast")->value();
        double slow = graph_.require("sma_slow")->value();
        double atr = graph_.require("atr")->value();
        
        // Entry
        if (fast > slow && !hasLongPosition(sym)) {
            emitMarketBuy(qty_);
        }
        // Exit
        else if (fast < slow && hasLongPosition(sym)) {
            emitMarketSell(positionSize(sym));
        }
    }
    
    void onSymbolTrade(flox::SymbolId sym, const flox::Trade& trade) override {
        // Intrabar risk management (optional)
    }
};
```

---

## Summary Table: What to Use When

| Task | Tool/Class | Doc Path |
|------|-----------|----------|
| Add indicator | C++ class + registry.def | `docs/how-to/add-an-indicator.md` |
| Cache indicators | IndicatorGraph | `docs/how-to/indicator-graph.md` |
| Write strategy | Inherit `Strategy` | `docs/how-to/strategy-classes.md` |
| Multi-timeframe | BarMatrix + MultiTimeframeAggregator | `docs/how-to/multi-tf-context.md` |
| CSV backtest | `BacktestRunner::run_csv()` | `docs/how-to/backtest.md` |
| Binary bar backtest | `MmapBarStorage` + `run_bars()` | `docs/reference/api/backtest/mmap_bar_storage.md` |
| Grid search | `BacktestOptimizer<Params,Grid>` | `docs/how-to/grid-search.md` |
| Walk-forward | `WalkForwardRunner` | `docs/how-to/walk-forward.md` |
| Paper trading | `PaperBroker` + `SimulatedExecutor` | `docs/how-to/paper-trading.md` |
| Record tick data | `flox tape record` / `MarketDataRecorder` | `docs/how-to/tape-record.md` |
| Realistic fills | Slippage models + Queue simulation | `docs/how-to/backtest-realistic-fills.md` |
| Lookahead detection | `flox lint lookahead` | `docs/how-to/lookahead-detector.md` |
| Multi-symbol | `partitionBySymbol` / `forEachSymbolParallel` | `docs/how-to/multi-symbol-indicators.md` |
| Multi-exchange | CEX connectors | `docs/how-to/multi-exchange-trading.md` |
| Portfolio risk | `PortfolioRiskAggregator` | `docs/how-to/portfolio-risk.md` |
