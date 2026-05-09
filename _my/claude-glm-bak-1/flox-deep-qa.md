# FLOX Deep Q&A — Comprehensive Answers

**Date**: 2026-05-09
**Scope**: All questions about FLOX C++/Python usage, data handling, strategies, live trading, Oryon integration, and trading concepts.

---

## Q1: Fork vs Separate Repo for Custom C++ Code

### Recommendation: Start Inside a Forked FLOX Repo

**Why fork is easier to start with:**

1. **Access to internal headers.** Your custom indicators need `flox/indicator/` internals, `Bar` struct internals, `Price`/`Quantity` fixed-point types. These are all available without extra CMake setup.

2. **Build system integration.** FLOX's CMake already compiles indicators, links against the engine, runs tests. Adding a new file under `flox/indicator/` or `flox/strategy/` picks up existing build rules automatically via glob patterns:
   ```cmake
   # Already exists in flox's CMake - picks up new .cpp files
   file(GLOB_RECURSE INDICATOR_SOURCES "indicator/*.cpp")
   ```

3. **Testing.** FLOX's test infrastructure (`BacktestRunner`, `SimulatedExecutor`, `ReplayConnector`) is immediately available. No cross-repo linking needed.

4. **Same code = all bindings.** Add an indicator in C++ under `flox/indicator/`, and pybind11 bindings in `flox/python/bindings/` expose it to Python automatically. Same for Node.js.

**When to move to a separate repo:**

- When your strategy code is stable and you want to version it independently
- When you need to keep proprietary logic separate from the open-source FLOX fork
- When multiple projects share the same custom indicators

**Migration path**: Start in the fork. When the code is stable, extract it into a separate CMake project that links against `flox::flox` as a dependency.

**Directory structure inside the fork:**
```
flox/                          # Your forked FLOX repo
├── flox/
│   ├── indicator/
│   │   ├── donchian.h         # Your custom indicators
│   │   ├── donchian.cpp
│   │   ├── keltner.h
│   │   ├── keltner.cpp
│   │   └── ...
│   └── strategy/
│       └── examples/           # Your custom strategies
│           ├── donchian_breakout.h
│           ├── donchian_breakout.cpp
│           └── ...
├── connectors/                 # Existing connectors (Bybit, Bitget, etc.)
├── demo/                       # Add your demos here
└── _my/                        # Your private working directory
    ├── strategies/             # Strategy implementations
    ├── data/                   # Downloaded market data
    └── results/                # Backtest results
```

---

## Q2: Can FLOX Work with Historical 1M Data?

### Yes. FLOX Can Work with 1-Minute (and any OHLCV) Data.

FLOX has **two paths** for consuming data:

**Path 1: CSV OHLCV reader (simplest)**
```cpp
// C++ - reads CSV directly
auto reader = replay::createCsvOhlcvReader("btcusdt_1m.csv", symbolId);
BacktestRunner runner(config);
runner.setStrategy(&strategy);
auto result = runner.run(*reader);
```

```python
# Python - reads CSV directly
bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
bt.set_strategy(my_strategy)
stats = bt.run_csv("btcusdt_1m.csv", "BTCUSDT")
```

**Path 2: Pre-aggregated bars via `run_bars()` (faster for repeated sweeps)**
```python
# Python - pass numpy arrays directly
bt.run_bars(
    start_time_ns = ts_start_arr.astype(np.int64),
    end_time_ns   = ts_end_arr.astype(np.int64),
    open=opens, high=highs, low=lows, close=closes, volume=vols,
    symbol="BTCUSDT",
)
```

```cpp
// C++ - memory-mapped bar storage (zero-copy)
MmapBarStorage storage("/data/BTCUSDT/bars");
MmapBarReplaySource source(storage, symbol_id);
source.replay([&](const BarEvent& ev) { strat.onBar(ev); });
```

**Path 3: Convert 1M data to .floxlog format for ReplayConnector**
The `.floxlog` binary format supports trades and book updates. You can convert OHLCV data into synthetic trades and replay them through the full FLOX pipeline. This is overkill for 1M data but useful if you want to use FLOX's bar aggregator to build different bar types from 1M "ticks."

### FLOX Does NOT Require Tick Data

Tick data (`.floxlog`) is only needed for:
- Non-standard bar types (volume bars, dollar bars, renko, range bars) aggregated from ticks
- Queue simulation for limit orders
- Order book reconstruction
- Realistic slippage modeling with `VOLUME_IMPACT`

For bar-driven strategies on 1M, 5M, 1H, 4H, 1D data, **CSV or numpy arrays are sufficient.**

---

## Q3: Can FLOX Work with CSV/Parquet Broker Data?

### Yes, via CSV Reader. Parquet needs conversion.

**CSV: Direct support**
```
CSV format expected: timestamp,open,high,low,close,volume
```
The `createCsvOhlcvReader` (C++) and `run_csv` (Python) read this directly.

**Parquet: Convert to CSV or numpy arrays**
FLOX doesn't have a built-in parquet reader for backtesting. Convert parquet to CSV or load with pandas/polars and pass as numpy arrays:

```python
import polars as pl
import numpy as np

df = pl.read_parquet("btcusdt_4h.parquet")
# Convert timestamp to nanoseconds
ts_start = df["open_time"].to_numpy().astype(np.int64)
ts_end = df["close_time"].to_numpy().astype(np.int64)

bt.run_bars(
    start_time_ns=ts_start,
    end_time_ns=ts_end,
    open=df["open"].to_numpy(),
    high=df["high"].to_numpy(),
    low=df["low"].to_numpy(),
    close=df["close"].to_numpy(),
    volume=df["volume"].to_numpy(),
    symbol="BTCUSDT",
)
```

**Can you build FLOX bars from 1M candles instead of ticks?**

Yes. Treat each 1M candle as a "trade" and feed it through the bar aggregator:
```python
# Convert 1M candles into synthetic trades, then aggregate to 4H bars
for _, row in df_1m.iterrows():
    aggregator.onTrade(make_trade(symbol, row.close, row.volume, row.timestamp))
```

This gives you FLOX's 6 bar aggregation types (time, tick, volume, renko, range, heikin-ashi) from 1M data without needing tick data. The approximation is very close for strategies on 1H+ timeframes.

**FLOX native formats for preprocessed data:**

| Format | Use Case | Access Pattern |
|--------|----------|----------------|
| CSV | Quick prototyping | `run_csv()` |
| numpy arrays | Parameter sweeps | `run_bars()` |
| `.floxlog` | Tick-level replay | `ReplayConnector`, `MmapReader` |
| `bars_*.bin` (mmap) | Zero-copy repeated sweeps | `MmapBarStorage` |
| `.floxrun` | Strategy trace for debugging | Replay viewer |

---

## Q4: How FLOX Indicators Are Calculated

### Two Modes: Batch (compute) and Streaming (update)

From `docs/explanation/indicators.md`:

> Indicators work in two modes: **batch** (pass an array, get an array back) and **streaming** (call `.update()` each tick, check `.ready` before reading `.value`).

**Batch mode:**
```cpp
// C++ - batch computation on precomputed bars
auto sma_values = SMA(50).compute(indicator::close(bars));
auto ema_values = EMA(200).compute(indicator::close(bars));
```

```python
# Python - batch computation
closes = np.array([100.0, 101.0, ...])
sma = flox.SMA(50).compute(closes)  # Returns numpy array
```

**Streaming mode:**
```cpp
// C++ - streaming inside strategy
class MyStrategy : public Strategy {
    flox::indicator::SMA _fast{10};
    flox::indicator::SMA _slow{30};

    void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override {
        double f = _fast.update(ev.trade.price.toDouble());
        double s = _slow.update(ev.trade.price.toDouble());
        if (!_slow.ready()) return;
        if (f > s && ctx.isFlat()) emitMarketBuy(symbol(), _size);
    }
};
```

```python
# Python - streaming
class SMAStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(30)

    def on_trade(self, ctx, trade):
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if fv is None or sv is None or not self.slow.ready:
            return
        if fv > sv and ctx.is_flat():
            self.market_buy(0.01)
```

**Indicators calculate on TRADES (raw events), not bars.** The `update()` method receives individual trade prices. But when you use `on_bar` callbacks, each bar's close price is treated as a single "trade" for indicator updates.

### Should You Use the Indicator Graph?

**Yes, for complex strategies.** The Indicator Graph (`indicator-graph.md`) caches shared computations:

```python
from flox_py.composite import when, TIME_BARS

H4_NS = 4 * 3600 * 1_000_000_000

class TrendFollow(flox.Strategy):
    def setup(self):
        self.entry = (
            when(self, btc_id, TIME_BARS, H4_NS).ema(50)
            > when(self, btc_id, TIME_BARS, H4_NS).ema(200)
        ) & (
            when(self, btc_id, TIME_BARS, H4_NS).rsi(14) < 30
        )

    def on_bar(self, ctx, bar):
        if self.entry.is_ready() and self.entry.value():
            self.market_buy(0.01)
```

Benefits of Indicator Graph:
- **Automatic warmup tracking**: `.is_ready()` returns False until all leaf indicators have enough data
- **Shared computation**: If EMA(50) is used in multiple conditions, it's computed once
- **Multi-TF support**: Cross-timeframe conditions in one expression
- **Available in all bindings**: Python, Node.js, Codon, QuickJS

**When to NOT use the Indicator Graph:**
- Simple single-TF strategies where you have 2-3 indicators
- When you need custom indicator classes that aren't in FLOX's built-in set
- When you want maximum control over computation order

---

## Q5: Tick Data Availability from Brokers

### Most Crypto Exchanges Do NOT Provide 5-6 Years of Tick Data via API

**What's typically available via exchange APIs:**

| Exchange | Free OHLCV History | Tick/Trade History | Order Book History |
|----------|-------------------|-------------------|-------------------|
| Binance | ~1-4 years (1M-1D) | ~3-12 months via API | No |
| Bybit | ~1-2 years | Limited | No |
| Bitget | ~1-2 years | Limited | No |

**Third-party historical data providers:**

| Provider | Data Type | Coverage | Cost |
|----------|-----------|----------|------|
| Tardis.dev | Trades + Order Book | 5+ years for major pairs | Paid |
| CoinGlass | OHLCV + OI + Funding + Liquidations | 3-5 years | Paid |
| Kaiko | Trades + Order Book | 5+ years | Enterprise |
| CryptoCompare | OHLCV + Trades | 5+ years | Freemium |

### How to Make FLOX Work Without Tick Data

**Approach 1: Use 1M OHLCV data (recommended for 4H strategies)**
```python
# Download 1M data from Binance (free, via ccxt)
# Feed to FLOX as trades (1 bar = 1 synthetic trade)
bt.run_csv("btcusdt_1m.csv", "BTCUSDT")
```

**Approach 2: Build synthetic ticks from 1M bars**
```python
# Within each 1M bar, generate synthetic ticks that respect OHLCV
for _, row in df_1m.iterrows():
    # Generate 4 synthetic ticks per bar: open→high, high→low, low→close, close
    synthetic_trades = interpolate_bar(row)
    for trade in synthetic_trades:
        aggregator.onTrade(trade)
```

**Approach 3: Use Tardis.dev for real tick data**
```python
# Tardis provides tick-level data in their format
# Convert to .floxlog using FLOX's BinaryLogWriter
import tardis_dev

for trade in tardis_dev.replay("binance", "BTCUSDT", "2020-01-01", "2025-01-01"):
    writer.writeTrade(trade)
```

**For your 4H strategies: 1M OHLCV data is sufficient.** The edge is in multi-day trend regime detection, not microsecond price formation.

---

## Q6: How to Update Historical Data with New Deltas

### Three Approaches, Ranked by Complexity

**Approach 1: Append New CSV/Parquet (Simplest)**

```python
# Daily download job (cron/cloud function)
import ccxt
import polars as pl

exchange = ccxt.binance()
symbol = "BTC/USDT"

# Get last known timestamp from existing data
existing = pl.read_parquet("btcusdt_1m.parquet")
last_ts = existing["timestamp"].max()

# Fetch new data since last timestamp
new_bars = exchange.fetch_ohlcv(symbol, "1m", since=last_ts, limit=1000)
new_df = pl.DataFrame(new_bars, schema=["timestamp", "open", "high", "low", "close", "volume"])

# Append
updated = pl.concat([existing, new_df])
updated.write_parquet("btcusdt_1m.parquet")
```

**Approach 2: Live Recording Strategy (FLOX native)**

```python
# Run a FLOX strategy that records all incoming data to .floxlog
import flox_py as flox

class DataRecorder(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.writer = flox.BinaryLogWriter("live_capture.floxlog")

    def on_trade(self, ctx, trade):
        self.writer.write_trade(trade)

    def on_book(self, ctx, book):
        self.writer.write_book(book)
```

This is the approach FLOX is designed for. Run the recorder strategy constantly online, accumulating `.floxlog` segments. For backtesting, replay the recorded data.

**Approach 3: Periodic Backfill with Conversion**

```bash
# Daily cron: download, convert to FLOX format, append to segment directory
flox tape record --exchange binance --symbols BTCUSDT --output /data/raw/
```

### Recommended Workflow for Your Case

```
Daily Update Pipeline:
1. Cron job runs at 00:05 UTC
2. Downloads 1M OHLCV since last update (ccxt)
3. Appends to master parquet file
4. Converts new bars to FLOX format if needed
5. Updates MmapBarStorage for fast backtesting

Weekly Update Pipeline:
1. Download funding rates, OI data
2. Merge with OHLCV data
3. Recompute Oryon feature pipeline if DAG changed
4. Run backtests on updated data
```

---

## Q7: Live Trading Data Feed and Warmup

### How Live Trading Gets Data

FLOX's live data flow:
```
Exchange WebSocket → Connector (Bybit/Bitget/Hyperliquid) → TradeBus/BookBus
                                                                    ↓
                                                            Strategy.onTrade()
                                                            Strategy.onBookUpdate()
```

### Warmup: Historical Data for Live Strategies

**Problem**: A strategy with EMA(200) needs 200 bars of history before it can generate signals. On live start, you have zero bars.

**FLOX Approach**: FLOX doesn't have built-in historical warmup for live strategies. You must feed historical data through the strategy before connecting to the live feed.

```python
# Warmup pattern in FLOX
class LiveStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        self.fast = flox.SMA(10)
        self.slow = flox.SMA(200)  # Needs 200 bars to be ready

    def on_trade(self, ctx, trade):
        fv = self.fast.update(trade.price)
        sv = self.slow.update(trade.price)
        if not self.slow.ready:
            return  # Skip signals during warmup
        # ... trading logic

# 1. Warmup from historical data
warmup_bars = load_parquet("btcusdt_1m.parquet").tail(500)  # 500 bars for safety
runner = flox.Runner(registry, on_signal=handle_signal)
runner.add_strategy(LiveStrategy([btc]))
runner.start()

for _, bar in warmup_bars.iterrows():
    runner.on_trade(btc, bar.close, bar.volume, True, bar.timestamp)

# 2. Now connect to live feed
for trade in live_websocket():
    runner.on_trade(btc, trade.price, trade.qty, trade.is_buy, trade.ts)
```

### Oryon Warmup vs FLOX Warmup

**Oryon warmup:**
```python
# Oryon: same pipeline, different mode
pipeline = FeaturePipeline(features=[Ema(200), Skewness(50)])

# Batch warmup from historical data
history_df = pd.read_parquet("btcusdt_4h.parquet").tail(500)
for _, row in history_df.iterrows():
    pipeline.update([row.close, row.high, row.low])

# Now pipeline is warm - switch to live
for bar in live_bars:
    features = pipeline.update([bar.close, bar.high, bar.low])
    if features_ready:
        decision = strategy.evaluate(features)
```

**Key difference**: Oryon guarantees that `run_research()` and `update()` produce identical results. So you can warmup from historical batch data, and the live `update()` path produces the same feature values.

**FLOX warmup**: No such guarantee is built-in. FLOX indicators are simpler (no DAG), so warmup is just feeding data through `.update()` calls.

**For your Strategy Brain architecture**: Use Oryon for warmup because:
1. Same pipeline in batch and streaming = identical features
2. Feature DAG resolves dependencies automatically
3. `reset()` between folds for walk-forward
4. Custom features with parity guarantees

---

## Q8: Oryon + FLOX C++ Integration

### Can You Call Oryon from C++ FLOX Strategies?

**Short answer**: Not directly, but you can bridge them.

**Oryon is Rust** (PyO3 bindings → Python). **FLOX is C++** (pybind11 bindings → Python).

They meet in Python:

```python
# Python integration layer
import flox_py as flox
import oryon

class OryonFloxStrategy(flox.Strategy):
    def __init__(self, symbols):
        super().__init__(symbols)
        # Oryon pipeline for features
        self.pipeline = oryon.FeaturePipeline([
            oryon.Ema(50),
            oryon.Ema(200),
            oryon.ShannonEntropy(20),
        ])
        # FLOX indicators for execution
        self.rsi = flox.RSI(14)
        self.bb = flox.BollingerBands()

    def on_trade(self, ctx, trade):
        # Feed both systems
        ema50 = self.pipeline.update([trade.price])
        rsi = self.rsi.update(trade.price)
        bb = self.bb.update(trade.price)

        # Combine Oryon features + FLOX indicators
        if ema50 and ema50[0] > ema50[1]:  # Trend confirmation from Oryon
            if rsi < 30 and ctx.is_flat():   # RSI from FLOX
                self.market_buy(0.01)
```

### Does It Make Sense to Use Oryon from C++?

**For pure C++ strategies: Not really.** FLOX's C++ indicators are faster (no FFI overhead) and simpler. Use FLOX indicators for everything in C++.

**When Oryon adds value (Python layer):**

| Feature | FLOX Built-in | Oryon |
|---------|---------------|-------|
| Batch/streaming parity | Per indicator (manual) | Guaranteed by construction |
| Feature DAG | No | Yes (automatic dependency resolution) |
| Statistical features (entropy, skewness, kurtosis) | No | Yes |
| Target engineering (FutureReturn) | No | Yes |
| Custom feature parity testing | Manual | Built-in (vectorized reference) |
| Pandas/Polars adapters | No | Yes |
| Multi-symbol features | Manual | Via FeaturePipeline |

**Recommendation for your architecture:**
- **C++ FLOX strategies**: Use FLOX native indicators only
- **Python strategy layer**: Use Oryon for features + FLOX for execution
- **Strategy Brain pattern**: Strategy declares features via Oryon, Python bridge feeds both systems

### How Oryon Features Differ from FLOX Native C++ Features

```
FLOX native C++ features:
  - Implemented in C++ (zero-copy, no GIL)
  - Simple: update() → value, compute() → array
  - No DAG, no automatic dependency resolution
  - Indicators: SMA, EMA, RSI, MACD, BB, ADX, ATR, Supertrend, VWAP, CVD
  - Best for: execution-speed indicators inside onTrade()

Oryon features:
  - Implemented in Rust (PyO3, ~511ns FFI overhead)
  - DAG-based composition with automatic resolution
  - Batch/streaming parity guaranteed
  - Features: EMA, SMA, KAMA, returns, vol, skewness, kurtosis, entropy, ADF
  - Best for: research features that must match between backtest and live
```

---

## Q9: How to Feed Data to Backtesting

### FLOX Backtest Data Input Methods

**Method 1: CSV file**
```python
stats = bt.run_csv("data/btcusdt_1m.csv", "BTCUSDT")
```

**Method 2: Numpy arrays (for precomputed indicators)**
```python
bt.run_bars(
    start_time_ns=ts_start, end_time_ns=ts_end,
    open=opens, high=highs, low=lows, close=closes, volume=vols,
    symbol="BTCUSDT",
)
```

**Method 3: FLOX .floxlog replay (tick-level)**
```cpp
auto reader = replay::createMultiSegmentReader("./data", filter);
BacktestResult result = runner.run(*reader);
```

**Method 4: MmapBarStorage (zero-copy bar replay)**
```cpp
MmapBarStorage storage("/data/BTCUSDT/bars");
MmapBarReplaySource source(storage, symbol_id);
auto result = runner.run(source);
```

### How to Provide Precalculated Indicators to Backtesting

**Strategy holds its own indicators (streaming)**:
```cpp
class DonchianBreakout : public Strategy {
    DonchianChannel _donch{20};  // Streaming indicator
    ATR _atr{14};

    void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override {
        _donch.update(ev.trade.price);
        _atr.update(ev.trade.price);
        // Strategy uses indicator state directly
    }
};
```

**Precomputed indicator columns passed to strategy:**
If you precompute features (e.g., via Oryon), store them alongside OHLCV and access by timestamp:

```python
# Precompute features
features_df = oryon.run_features_pipeline_pandas(pipeline, ohlcv_df)
# features_df has columns: close, ema_50, ema_200, entropy_20, ...

# Strategy reads precomputed features
class PrecomputedStrategy(flox.Strategy):
    def __init__(self, symbols, feature_df):
        super().__init__(symbols)
        self.features = feature_df
        self.bar_index = 0

    def on_trade(self, ctx, trade):
        row = self.features.iloc[self.bar_index]
        self.bar_index += 1
        if row['ema_50'] > row['ema_200']:
            self.market_buy(0.01)
```

### Indicator Graph in Backtesting and Live

The Indicator Graph works identically in both modes:
```python
# Same code for backtesting and live
self.entry = (
    when(self, btc_id, TIME_BARS, H4_NS).ema(50)
    > when(self, btc_id, TIME_BARS, H4_NS).ema(200)
)

# Backtesting: bars come from CSV replay
# Live: bars come from exchange WebSocket
# Indicator Graph doesn't care about the source
```

---

## Q10: Live Trading Memory and Warmup

### Memory Usage in Live Trading

**Per-symbol state (from `SymbolContext`):**
```
NLevelOrderBook<512>  → ~4KB per symbol (dominates)
Position state        → ~64 bytes
Price tracking        → ~32 bytes
Timestamp             → 8 bytes
Total per symbol:     → ~4.1KB
```

**Strategy memory:**
```
Indicator state       → Depends on window size
  SMA(200)            → ~1.6KB (200 doubles)
  EMA(200)            → ~16 bytes (stateless)
  RSI(14)             → ~128 bytes
  Donchian(55)        → ~440 bytes (deque of 55 prices)
Streaming brain state → Strategy-specific (typically <1KB)
Oryon pipeline        → ~1-10KB per pipeline (fixed, regardless of data)
```

**Total for 1 symbol with 10 indicators:** ~10KB
**Total for 40 symbols:** ~400KB
**FLOX engine overhead:** ~1-5MB (event buses, pools, connectors)

**Bottom line: Live trading uses <50MB total for 40 symbols with complex strategies.**

### Best Practice for Warmup

```python
# Recommended warmup sequence:
# 1. Load last N bars of historical data (N = max indicator window × 2)
# 2. Feed through strategy's streaming indicators
# 3. Verify indicators are ready
# 4. Connect to live feed

WARMUP_BARS = 500  # Enough for EMA(200) with margin

def warmup_strategy(strategy, symbol, registry):
    # Load historical data
    df = pd.read_parquet(f"data/{symbol}_1m.parquet").tail(WARMUP_BARS)

    runner = flox.Runner(registry, lambda sig: None)
    runner.add_strategy(strategy)
    runner.start()

    # Feed historical bars as trades
    for _, row in df.iterrows():
        runner.on_trade(symbol, row.close, row.volume, True, row.timestamp)

    # Verify warmup
    if not strategy.slow.ready:
        raise RuntimeError("Warmup failed - not enough data")

    return runner  # Continue with live feed
```

---

## Q11: Trading Concepts and FLOX Entities

### Trading 101: The Entities and Their Relationships

#### Participants in Trading

```
Market Participants:
├── Retail Traders       → Small size, often emotional, provide liquidity
├── Institutional Traders → Large size, systematic, consume liquidity
├── Market Makers         → Provide bid/ask quotes, earn the spread
├── Arbitrageurs          → Exploit price differences across venues
├── Algorithmic Traders   → Execute rules-based strategies (YOU are here)
└── Exchanges             → Match buyers and sellers, earn fees
```

#### Core Entities Explained

**Price**: The agreed-upon value at which a trade happens. Determined by supply and demand.

**Bid**: The highest price a buyer is willing to pay right now.
**Ask (Offer)**: The lowest price a seller is willing to accept right now.
**Spread**: Ask - Bid. The gap between buyer and seller. This is the cost of immediate execution.

```
Order Book (L2 Data):
  ASKS (sellers)          BIDS (buyers)
  Price    Quantity       Price    Quantity
  101.00   5.0            100.00   10.0    ← Best Bid
  100.50   8.0            99.50    15.0
  100.25   3.0            99.00    20.0
  ↑                                  ↑
  Best Ask = 100.25        Best Bid = 100.00
  Spread = 0.25
  Mid Price = 100.125
```

**Order**: An instruction to buy or sell. Types:
- **Market Order**: Execute immediately at best available price. You pay the spread.
- **Limit Order**: Execute only at specified price or better. You wait.
- **Stop Order**: Trigger a market/limit order when price reaches a threshold.
- **Trailing Stop**: Stop that follows the price at a fixed distance.

**Trade**: A completed transaction. One buyer, one seller, agreed price and quantity.

**Position**: Your current holdings. Long (you bought, profit if price goes up) or Short (you sold borrowed asset, profit if price goes down).

**Slippage**: The difference between expected price and actual fill price. Caused by:
- Market impact (your order moves the price)
- Latency (price changed between decision and execution)
- Spread (you cross the spread on market orders)

**Volume**: Number of units traded in a period.

**Order Book (L2)**: List of all outstanding bids and asks at each price level.

**L1 Data**: Top of book only (best bid + best ask).
**L2 Data**: Full order book depth (all price levels).
**L3 Data**: Full order book with individual order IDs (rare).

**Imbalance**: When buy volume significantly exceeds sell volume (or vice versa) at a price level. Indicates directional pressure.

**Liquidity**: The ease of buying/selling without moving the price. More orders in the book = more liquid.

### How These Entities Relate in FLOX

```
FLOX Entity Mapping:

Trade (struct)              ← One executed transaction
  .symbol                   ← Which instrument (SymbolId)
  .price                    ← Execution price (Price, fixed-point)
  .quantity                 ← Size (Quantity, fixed-point)
  .isBuy                    ← Was taker the buyer?
  .exchangeTsNs             ← When it happened (UnixNanos)

BookUpdate (struct)         ← Order book change
  .bids                     ← Bid levels (PMR vector)
  .asks                     ← Ask levels (PMR vector)
  .type                     ← SNAPSHOT or DELTA

Order (struct)              ← Your instruction to exchange
  .side                     ← BUY or SELL
  .type                     ← MARKET, LIMIT, STOP_MARKET, etc.
  .price                    ← Limit price
  .quantity                 ← Size
  .triggerPrice             ← For stop/TP orders
  .trailingOffset           ← For trailing stops

Position (via PositionTracker)
  .quantity                 ← Net position (positive=long, negative=short)
  .avgEntryPrice            ← VWAP entry price
  .realizedPnl              ← Closed profit/loss
```

### Price Inefficiency: Why Trading Can Be Profitable

Price inefficiency exists because:
1. **Information asymmetry**: Some participants know more than others
2. **Speed asymmetry**: Some react faster than others
3. **Behavioral biases**: Fear/greed cause overshooting and mean reversion
4. **Liquidity provision**: Market makers earn the spread for providing liquidity
5. **Structural edges**: Funding rates, basis premiums, liquidation cascades

Your strategies exploit these inefficiencies:
- **Trend following** (Donchian, Supertrend): Behavioral momentum
- **Mean reversion** (RSI2, RSI_BB_MR): Overshooting and snap-back
- **Breakout** (Keltner, Bollinger): Volatility compression → expansion
- **Crypto-specific** (funding carry, basis premium): Structural market mechanics

### FLOX Strategy Workflow

```
Live Strategy Workflow:

1. Data arrives via connector (WebSocket)
2. FLOX engine dispatches TradeEvent / BookUpdateEvent
3. Strategy receives event via callback:
   onTrade() → Every trade
   onBookUpdate() → Every book change
   onBar() → Every completed bar (from bar aggregator)

4. Strategy updates indicators
5. Strategy evaluates conditions
6. Strategy emits signals:
   emitMarketBuy() → Buy now at market
   emitMarketSell() → Sell now at market
   emitLimitBuy() → Buy at specific price
   emitStopMarket() → Sell if price drops to X
   emitTrailingStop() → Sell if price drops X% from peak
   emitClosePosition() → Close everything

7. Executor handles the order
8. Order events flow back:
   onOrderSubmitted → Order sent
   onOrderAccepted → Exchange acknowledged
   onOrderFilled → Trade completed
   onOrderRejected → Exchange refused
```

---

## Q11b: Accessing Previous Bars in Strategy

### Three Ways to Access Historical Data in FLOX

**Method 1: Manual Storage (Simple, Common)**

```cpp
class SmaCrossover : public Strategy {
    std::deque<double> _prices;  // Store last N prices
    size_t _slow;

    void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override {
        _prices.push_back(ev.trade.price.toDouble());
        if (_prices.size() > _slow) _prices.pop_front();
        if (_prices.size() < _slow) return;  // Warmup

        double fast_sma = sma(_fast), slow_sma = sma(_slow);
        // ... trading logic
    }
};
```

**Method 2: BarMatrix for Multi-Timeframe (Recommended for MTF)**

```cpp
// From multi_timeframe_demo.cpp
class MTFMomentumStrategy : public IMarketDataSubscriber {
    BarMatrix<256, 4, 64>* _matrix;  // Stores last 64 bars per timeframe

    void onBar(const BarEvent& ev) override {
        // Access current and previous bars
        const Bar* h1 = _matrix->bar(_symbol, timeframe::H1, 0);      // Current H1
        const Bar* h1_prev = _matrix->bar(_symbol, timeframe::H1, 1); // Previous H1
        const Bar* m5 = _matrix->bar(_symbol, timeframe::M5, 0);      // Current M5
        const Bar* m1 = _matrix->bar(_symbol, timeframe::M1, 0);      // Current M1

        if (!h1 || !h1_prev || !m5 || !m1) return;  // Warmup check

        // Use bars directly
        bool h1Bullish = h1->close.raw() > h1_prev->close.raw();
    }
};
```

**Method 3: `last_n_closed_bars` (Python composite conditions)**

```python
# The composite conditions DSL accesses bars automatically
self.entry = (
    when(self, btc_id, TIME_BARS, H4_NS).ema(50)
    > when(self, btc_id, TIME_BARS, H4_NS).ema(200)
)
# The DSL pulls bars from Strategy.last_n_closed_bars internally
```

### You Do NOT Need to Wait for Live Warmup Only

**You CAN warmup from historical data.** The pattern is:

1. Load historical bars into BarMatrix or deque
2. Feed them through indicators before connecting to live feed
3. Indicators will be "warm" when live data starts

For MTF strategies, use `MultiTimeframeAggregator` + `BarMatrix`:
```cpp
MultiTimeframeAggregator<4> aggregator(&bus);
aggregator.addTimeInterval(std::chrono::seconds(60));    // M1
aggregator.addTimeInterval(std::chrono::seconds(300));   // M5
aggregator.addTimeInterval(std::chrono::seconds(3600));  // H1

// Feed historical trades to warm up all timeframes
for (const auto& trade : historicalTrades) {
    aggregator.onTrade(trade);
}
// Now BarMatrix has bars for all timeframes
```

---

## Q12: Bar Close vs Intrabar Decision Making

### When to Decide: on_bar vs on_trade vs on_book_update

**on_bar (Bar Close)**: Decide at the end of each bar period.
- **Best for**: Most systematic strategies (your 4H strategies)
- **Pros**: No intrabar noise, matches backtesting exactly, simple logic
- **Cons**: Late execution (wait for bar close), missed intrabar opportunities

**on_trade (Every Tick)**: Decide on every trade.
- **Best for**: Market making, HFT, latency arbitrage
- **Pros**: Fastest reaction, capture intrabar moves
- **Cons**: Much higher processing load, noise, backtesting complexity

**on_book_update (Order Book Changes)**: Decide on every book change.
- **Best for**: Liquidity detection, iceberg detection, queue position tracking
- **Pros**: See supply/demand shifts before they execute
- **Cons**: Highest data volume, most complex logic

### Recommended Approach for Your Strategies

**Combine on_bar for decisions with on_trade for indicator updates:**

```cpp
class HybridStrategy : public Strategy {
    DonchianChannel _donch{20};
    ATR _atr{14};

    // Called on every trade - update indicators
    void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override {
        _donch.update(ev.trade.price);
        _atr.update(ev.trade.price);
    }

    // Called on bar close - make decisions
    void onBar(const BarEvent& ev) override {
        if (!_donch.ready() || !_atr.ready()) return;

        Price upper = _donch.upper();
        Price close = ev.bar.close;

        // Entry decision at bar close
        if (close.raw() > upper.raw() && ctx().isFlat()) {
            // Set stop loss at bar close
            Price stop = Price::fromRaw(close.raw() - _atr.value() * 3);
            emitMarketBuy(ev.symbol, _size);
            emitStopMarket(ev.symbol, Side::SELL, stop, _size);
        }
    }
};
```

### FLOX Batch vs Streaming (Similar to Oryon)

**Yes, FLOX has the same dual mode:**

```cpp
// Batch mode: compute indicator over array
auto sma_values = SMA(50).compute(close_array);  // Returns array

// Streaming mode: update per trade/bar
SMA sma(50);
for (double price : prices) {
    double val = sma.update(price);  // Returns value or NaN
    bool ready = sma.ready;          // True after enough data
}

// Reset between folds (walk-forward)
sma.reset();  // Clear internal state
```

This mirrors Oryon's pattern:
- Oryon: `run_research()` (batch) vs `update()` (streaming), `reset()` between folds
- FLOX: `compute()` (batch) vs `update()` (streaming), `reset()` between folds

**Key difference**: Oryon guarantees batch = streaming by construction (same Rust code path). FLOX indicators should produce the same results, but you need to verify this yourself.

---

## Q13: Complex Exit Modes in FLOX

### How to Implement All Exit Modes

FLOX provides native support for many exit types through its Order system:

**Built-in FLOX Exit Types:**

```cpp
// 1. ATR Trailing Stop
emitTrailingStop(symbol, Side::SELL, atrValue * multiplier, qty);

// 2. Fixed Stop Loss
emitStopMarket(symbol, Side::SELL, entryPrice - atrValue * slMult, qty);

// 3. Take Profit
emitTakeProfitMarket(symbol, Side::SELL, entryPrice + atrValue * tpMult, qty);

// 4. OCO (Stop Loss + Take Profit)
OCOParams params;
params.order1 = stopOrder;
params.order2 = tpOrder;
executor.submitOCO(params);

// 5. Trailing Stop Percentage
emitTrailingStopPercent(symbol, Side::SELL, 200, qty);  // 200 bps = 2%

// 6. Close Position (Market)
emitClosePosition(symbol);
```

### Implementation Patterns for Each Exit Mode

**Chandelier Exit** (position-aware trailing from best price since entry):
```cpp
class ChandelierTracker {
    Price _highWaterMark{};
    Price _lowWaterMark{};
    bool _inLong{false};

    void onTrade(Price price, ATR& atr, int lookback) {
        if (_inLong) {
            _highWaterMark = Price::fromRaw(std::max(_highWaterMark.raw(), price.raw()));
            _trailStop = Price::fromRaw(_highWaterMark.raw() - atr.value() * _mult);
        }
    }

    bool shouldExit(Price currentPrice, bool isLong) {
        if (isLong) return currentPrice.raw() < _trailStop.raw();
        return currentPrice.raw() > _trailStop.raw();
    }
};
```

**Bracket Exit** (fixed ATR SL + TP):
```cpp
void enterWithBracket(SymbolId symbol, Price entryPrice, double atrValue,
                       double slMult, double tpMult, Quantity qty) {
    // Enter
    emitMarketBuy(symbol, qty);

    // Stop Loss
    Price stopLoss = Price::fromRaw(entryPrice.raw() - (int64_t)(atrValue * slMult));
    emitStopMarket(symbol, Side::SELL, stopLoss, qty);

    // Take Profit
    Price takeProfit = Price::fromRaw(entryPrice.raw() + (int64_t)(atrValue * tpMult));
    emitTakeProfitMarket(symbol, Side::SELL, takeProfit, qty);
}
```

**Signal Reversal Exit** (exit when opposite signal fires):
```cpp
void onBar(const BarEvent& ev) {
    if (_currentSignal == Signal::LONG && newSignal == Signal::SHORT) {
        emitClosePosition(symbol);  // Close long
        emitMarketSell(symbol, qty);  // Enter short
    }
}
```

**Time Stop** (exit after N bars in trade):
```cpp
class TimeStopTracker {
    int _barsSinceEntry = 0;
    int _maxBars;

    void onBar() {
        if (_inPosition) {
            _barsSinceEntry++;
            if (_barsSinceEntry >= _maxBars) {
                emitClosePosition(symbol);
            }
        }
    }
};
```

**Breakeven Activation** (move stop to entry after R-multiple progress):
```cpp
void onTrade(Price price, Price entryPrice, double atrValue) {
    double rMultiple = (price.toDouble() - entryPrice.toDouble()) / atrValue;
    if (rMultiple >= 1.0 && !_breakevenActivated) {
        // Move stop to entry price
        emitCancelAll(symbol);  // Cancel old stop
        emitStopMarket(symbol, Side::SELL, entryPrice, qty);  // New stop at breakeven
        _breakevenActivated = true;
    }
}
```

### Exit Alpha Implementation Table

| Exit Alpha | FLOX Mechanism | Implementation |
|------------|---------------|----------------|
| ATR-based stop | `emitStopMarket()` | Calculate from entry ATR |
| Chandelier trail | Custom tracker + `emitStopMarket()` | Track highest/lowest since entry |
| Breakeven | Cancel old stop + `emitStopMarket()` at entry | R-multiple check |
| Time stop | Bar counter + `emitClosePosition()` | Simple counter |
| Signal reversal | `emitClosePosition()` + new entry | Signal comparison |
| Regime invalidation | Check regime on each bar | Regime filter in onBar |
| Volatility expansion | ATR spike detection | ATR percentile check |
| Funding accumulation | Track funding paid | Custom accumulator |
| CVD shift | Custom CVD tracker | FLOX CVD indicator |
| Liquidity cluster | Custom volume profile | VolumeProfile class |
| VPOC reach | VolumeProfile.poc() | Compare price to POC |

---

## Events Deep Dive: BookUpdateEvent, TradeEvent, BarEvent, OrderEvent

### The Four Core Events

**TradeEvent** — A trade happened on the exchange.
```cpp
// When: Every time a buyer and seller agree on a price
// Who creates it: Exchange connector or replay system
// Who receives it: BarAggregator, PnL trackers, strategies

struct TradeEvent {
    Trade trade;           // symbol, price, quantity, isBuy, timestamp
    int64_t seq;           // Exchange sequence number
    uint64_t tickSequence; // Internal bus sequence
    MonoNanos recvNs;      // Local receive time
    ExchangeId sourceExchange; // For CEX coordination
};
```

**BookUpdateEvent** — The order book changed.
```cpp
// When: Every time the order book changes (new/cancel/modify order)
// Who creates it: Exchange connector
// Who receives it: IOrderBook, SimulatedExecutor, strategies

struct BookUpdateEvent {
    BookUpdate update;     // symbol, bids, asks, type (SNAPSHOT or DELTA)
    int64_t seq;           // Exchange sequence
    int64_t prevSeq;       // Previous sequence (gap detection)
};
```

**BarEvent** — A bar was completed.
```cpp
// When: When BarAggregator completes a bar (time, volume, tick count, etc.)
// Who creates it: BarAggregator
// Who receives it: Strategies, BarMatrix

struct BarEvent {
    SymbolId symbol;
    BarType barType;       // Time, Tick, Volume, Renko, Range, HeikinAshi
    uint64_t barTypeParam; // Interval, threshold, etc.
    Bar bar;               // OHLCV data
};
```

**OrderEvent** — Your order changed state.
```cpp
// When: At every stage of the order lifecycle
// Who creates it: SimulatedExecutor (backtest) or exchange connector (live)
// Who receives it: PositionTracker, ExecutionTracker, OrderTracker

// Lifecycle: NEW → SUBMITTED → ACCEPTED → FILLED
//                                       → PARTIALLY_FILLED → FILLED
//                                       → CANCELED
//                                       → REJECTED
//            For conditional orders: PENDING_TRIGGER → TRIGGERED → FILLED
//            For trailing stops: TRAILING_UPDATED → TRIGGERED → FILLED

struct OrderEvent {
    OrderEventStatus status;  // NEW, SUBMITTED, ACCEPTED, FILLED, etc.
    Order order;              // The order
    Quantity fillQty;         // For partial fills
    Price fillPrice;          // Fill price
    std::string rejectReason;  // For REJECTED status
};
```

### Event Flow Diagram

```
Exchange / Replay
    │
    ├── TradeEvent ──────────→ TradeBus
    │                            ├── BarAggregator (consumes trades → produces bars)
    │                            ├── Strategy.onTrade()
    │                            ├── SimulatedExecutor.onTrade()
    │                            └── PnL Tracker
    │
    ├── BookUpdateEvent ─────→ BookUpdateBus
    │                            ├── IOrderBook.applyBookUpdate()
    │                            ├── Strategy.onBookUpdate()
    │                            └── SimulatedExecutor.onBookUpdate()
    │
    │   BarAggregator
    │       │
    │       └── BarEvent ────→ BarBus
    │                            ├── BarMatrix (stores history)
    │                            └── Strategy.onBar()
    │
    │   Strategy
    │       │
    │       └── emitMarketBuy() ──→ Signal
    │                                    │
    │                                    └── BacktestRunner.onSignal()
    │                                            │
    │                                            └── SimulatedExecutor.submitOrder()
    │                                                    │
    │                                                    └── OrderEvent ──→ OrderExecutionBus
    │                                                                        ├── PositionTracker
    │                                                                        ├── OrderTracker
    │                                                                        └── Strategy (via listener)
```

### EventDispatcher: How Events Route to Listeners

The `EventDispatcher<T>` provides **compile-time dispatch** — no virtual functions or dynamic casting:

```cpp
// Each event type declares its listener interface
TradeEvent::Listener = IMarketDataSubscriber
BookUpdateEvent::Listener = IMarketDataSubscriber
BarEvent::Listener = IMarketDataSubscriber
OrderEvent::Listener = IOrderExecutionListener

// EventDispatcher routes to the correct method
EventDispatcher<TradeEvent>::dispatch(event, subscriber)
    → subscriber.onTrade(event)

EventDispatcher<BarEvent>::dispatch(event, subscriber)
    → subscriber.onBar(event)

EventDispatcher<OrderEvent>::dispatch(event, listener)
    → event.dispatchTo(listener)  // Routes to correct onOrder* method
```

**Order: Command to do something. EventDispatcher: Listener for incoming events.**

Yes, exactly:
- **Order** = Your command ("buy 1 BTC at market")
- **OrderEvent** = Exchange's response ("your order was filled at $50,001")
- **EventDispatcher** = The routing mechanism that delivers events to the correct handler

### Complex Event Flow: MTF Strategy with Exits

```
1. Exchange sends TradeEvent
2. TradeBus publishes to subscribers:
   ├── BarAggregator.onTrade() → accumulates bar data
   ├── Strategy.onTrade() → updates streaming indicators
   └── SimulatedExecutor.onTrade() → updates market state

3. BarAggregator completes H1 bar → publishes BarEvent
4. BarBus delivers to:
   ├── BarMatrix → stores bar for history access
   └── Strategy.onBar() → evaluates entry conditions

5. Strategy detects entry → emitMarketBuy()
6. BacktestRunner converts signal to Order
7. SimulatedExecutor.submitOrder() → creates OrderEvent(SUBMITTED)
8. OrderExecutionBus delivers to:
   ├── OrderTracker.onSubmitted() → records state
   ├── PositionTracker → awaits fill
   └── Strategy (if listening) → can track order status

9. Next trade arrives → SimulatedExecutor fills order
10. OrderEvent(FILLED) published:
    ├── PositionTracker.onOrderFilled() → updates position
    ├── OrderTracker → marks complete
    └── Strategy → knows position is open

11. Strategy sets conditional exits:
    ├── emitStopMarket() → stop loss
    ├── emitTakeProfitMarket() → take profit
    └── emitTrailingStop() → trailing stop

12. Price hits stop → OrderEvent(TRIGGERED) → OrderEvent(FILLED)
13. Position closed → Strategy detects flat → can re-enter
```
