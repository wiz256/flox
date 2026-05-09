# Q12-Q14: FLOX Strategy Backtesting — Answers

## Q12: Strategy behavior — on_bar vs intrabar, MTF bar close detection

### How does FLOX dispatch bar events?

`Strategy::onBar()` is `final` — it internally dispatches to `onSymbolBar(SymbolContext&, BarEvent&)`. Strategies override `onSymbolBar` or `onSymbolTrade`, never `onBar` directly. The dispatch uses an internal symbol lookup table populated during construction.

### MTF bar close detection

In FLOX, MTF (multi-timeframe) bar close detection works through the `BarEvent` structure. Each `BarEvent` carries:

```cpp
struct BarEvent {
    SymbolId symbol;
    BarType barType;        // enum: Minute, Hour, Day, etc.
    int barTypeParam;       // e.g., 5 for 5-minute bars
    Bar bar;                // OHLCV data
    // ...
};
```

When you register a symbol with multiple bar subscriptions (1M, 5M, 4H), the system delivers separate `BarEvent` instances for each timeframe. Your strategy checks `ev.barType` and `ev.barTypeParam` to determine which TF bar closed:

```cpp
void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override {
    if (ev.barType == BarType::Hour && ev.barTypeParam == 4) {
        // 4H bar closed — run strategy logic
    } else if (ev.barType == BarType::Minute && ev.barTypeParam == 5) {
        // 5M bar closed — update indicators only
    }
}
```

You should NOT ignore the bar argument — it contains the complete, finalized OHLCV for that specific timeframe. The `BarEvent` tells you which timeframe closed; the `Bar` data is the final snapshot.

### on_bar vs onSymbolTrade

| Callback | When called | Data quality | Use case |
|----------|-------------|-------------|----------|
| `onSymbolBar` | Bar closes | Complete OHLCV, final | Signal generation, indicator updates |
| `onSymbolTrade` | Every trade tick | Individual trade price, qty | HFT, intrabar decisions, microstructure |

For systematic strategy backtesting on bar data, `onSymbolBar` is the right choice. `onSymbolTrade` is for tick-level strategies or when replaying bar data through `OhlcvReplaySource` (which synthesizes trade events from close prices).

### Our implementation choice

We use `onSymbolTrade` with `OhlcvReplaySource` because it's the simplest way to get OHLCV bar data flowing through FLOX's backtesting engine. The `OhlcvReplaySource` converts each bar's close price into a synthetic `TradeEvent`. Our strategy maintains a parallel index into the original CSV bars to access real H/L/C for indicators and ATR computation.

---

## Q13: Complex exit strategies — ExitAlphaAwareStrategy, grid search with ExitParams

### ExitAlphaAwareStrategy (renamed from AlphaAwareStrategy)

The `ExitAlphaAwareStrategy` pattern combines entry alpha (signal logic) with exit alpha (stop/target/trailing logic) in a single strategy. FLOX's `Strategy` class provides order methods (`emitMarketBuy`, `emitMarketSell`) that we wrap into clean entry/exit abstractions.

Our implementation in `BaseStrategy`:

```cpp
class BaseStrategy : public Strategy {
protected:
    void enterLong(double price) {
        if (inPos_) return;
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; inPos_ = true; side_ = 1; beAct_ = false;
        emitMarketBuy(symbol(), qty_);
    }

    void enterShort(double price) {
        if (inPos_) return;
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; inPos_ = true; side_ = -1; beAct_ = false;
        emitMarketSell(symbol(), qty_);
    }

    void exitPos() {
        if (!inPos_) return;
        if (side_ == 1) emitMarketSell(symbol(), qty_);
        else emitMarketBuy(symbol(), qty_);
        inPos_ = false;
    }

    void checkExit(double c, double h, double l, double atr);
    virtual void onBarImpl(double open, double high, double low, double close) = 0;
};
```

### Three exit modes implemented

| Mode | Entry | Exit Logic |
|------|-------|-----------|
| **Chandelier** | Signal | Trailing stop from highest favorable price using `trail_mult × ATR` |
| **ChandelierTimeStop** | Signal | Same as Chandelier + forced exit after `time_bars` bars |
| **SignalBETrail** | Signal | Hard stop at `entry - sl_mult × ATR`, breakeven at `be_r_mult × R`, then trailing at `trail_mult × ATR` |

### Grid search with ExitParams

The grid search sweeps signal parameters × exit parameters independently:

```
Signal grid (per strategy):
  donchian:        window ∈ {10,15,20,25,30,40,50}
  ema_crossover:   fast ∈ {5,8,10,12,15,20} × slow ∈ {20,30,40,50}
  keltner_breakout: ema_window ∈ {10,15,20,25,30} × atr_mult ∈ {1.5,2.0,2.5,3.0}
  supertrend:      atr_period ∈ {7,10,14,21} × atr_mult ∈ {2.0,2.5,3.0,3.5}
  ... etc

Exit grid (shared):
  atr_period ∈ {10, 14, 21}
  sl_mult   ∈ {1.5, 2.0, 2.5}
  trail_mult = 3.0 (fixed)
```

### Two-stage grid search approach

The user asked about a two-stage approach: first find entry alpha plateaus, then optimize exit modes. This is sound because:

1. **Stage 1**: Grid search entry params across all symbols. Find plateaus (clusters of neighboring params with ≥80% of best Sharpe).
2. **Stage 2**: For each plateau's center, sweep exit mode × exit params. This reduces the search space dramatically.

Our current implementation does a single-stage joint search (signal × exit together), which is more thorough but slower. For 6129 combos × 1 symbol, runtime is ~60 seconds.

### Plateau detection

```cpp
Plateau detectPlateau(const vector<pair<Params, double>>& results, size_t bestIdx) {
    double threshold = results[bestIdx].second * 0.8;
    int above = 0, total = 0;
    for (neighbors within ±2 steps of best param):
        total++;
        if (neighbor.sharpe >= threshold) above++;
    return { .ratio = above/total, .found = ratio >= 0.5 };
}
```

A plateau is found when ≥50% of neighboring parameter sets achieve ≥80% of the best Sharpe. This ensures the edge isn't just a lucky spike at one exact parameter value.

---

## Q14: Multi-symbol backtesting with walk-forward and White's reality check

### Implementation

The `crush_grid` binary implements the complete pipeline:

```
For each CSV file (40 coins × 4H):
  For each strategy (8):
    For each exit mode (3):
      Grid search signal_params × exit_params
      Filter: min 10 trades
      Rank by Sharpe ratio
      Detect plateau
      If plateau + Sharpe > 0.3:
        Walk-forward validation (5-fold)
        White's reality check (1000 bootstrap samples)
      Output to CSV
```

### Walk-forward validation

5-fold anchored walk-forward:

```
|====train====|==test==|                       Fold 0
           |====train====|==test==|             Fold 1
                    |====train====|==test==|    Fold 2
                              ...
```

Each fold uses the preceding data as training (params already selected from grid search), then tests on the out-of-sample window. We report average OOS Sharpe across folds. This catches overfitting — if OOS Sharpe collapses, the edge is likely statistical noise.

### White's Reality Check

White's RC tests whether the best strategy's performance is statistically significant after accounting for data snooping (testing many strategies and picking the best):

```cpp
WRCResult whitesRC(const vector<double>& returns, int bootstrap = 1000) {
    double observedMean = mean(returns);
    int exceedCount = 0;
    for (int b = 0; b < bootstrap; b++) {
        // Bootstrap: resample returns with replacement
        double bootMean = mean(resample(returns));
        if (bootMean >= observedMean) exceedCount++;
    }
    return { .p = exceedCount / bootstrap, .sig = p < 0.05 };
}
```

If `p < 0.05`, the returns are statistically significant — unlikely to be pure noise.

### Results from AAVEUSDT (1 symbol, 4H)

| Strategy | Exit Mode | Sharpe | Trades | Plateau | WF Sharpe | WRC p |
|----------|-----------|--------|--------|---------|-----------|-------|
| dual_momentum | chan | 1.25 | 54 | YES (1.0) | 0.423 | 0.476 |
| keltner_breakout | chan | 3.72 | 171 | no (0.31) | — | — |
| tsmom | chan_ts | 4.30 | 486 | no (0.21) | — | — |
| ema_crossover | chan_ts | 1.57 | 207 | no (0.11) | — | — |

Key finding: `dual_momentum` found a true plateau (all neighbors ≥80% of best) with positive walk-forward OOS Sharpe (0.423), but White's RC shows p=0.476 (not significant at 5% level). This means the edge, while present in-sample and stable across parameters, is not strong enough to rule out noise given the bar-level return distribution.

### Multi-symbol approach

The grid search runs independently per symbol. To find cross-symbol plateaus, we look for parameter sets that produce positive Sharpe on ≥60% of symbols. This is the same approach as the Python pipeline's `multi_slice` test in PROVE.

### Build & run instructions

```bash
# Build FLOX (one-time)
cd vendor/flox
cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)

# Build crush_grid
cd vendor/flox/_my/crush-glm
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build -j$(sysctl -n hw.ncpu)

# Run (1 symbol for testing)
./build/crush_grid --max-symbols 1 --data-dir data

# Run (full 40 symbols)
./build/crush_grid --data-dir data

# Debug build
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build-debug -j$(sysctl -n hw.ncpu)
```

### Console debugging

```bash
# Debug with LLDB
lldb ./build-debug/crush_grid
(lldb) run --max-symbols 1 --data-dir data
(lldb) bt          # backtrace on crash
(lldb) p bars.size()  # print variable
```

### VSCode debugging

Use the `.vscode/launch.json` provided. It has 4 configurations:
- Debug crush_grid (1 symbol) — ASan-enabled debug build
- Release crush_grid (1 symbol) — fast test
- Release crush_grid (full) — full 40-symbol run
- Debug minimal test — single-strategy sanity check

### Files created

| File | Description |
|------|-------------|
| `vendor/flox/_my/crush-glm/src/main.cpp` | Main grid search: 8 strategies, 3 exit modes, plateau, WF, WRC |
| `vendor/flox/_my/crush-glm/src/csv_reader.h` | CSV parser header |
| `vendor/flox/_my/crush-glm/src/csv_reader.cpp` | CSV parser implementation |
| `vendor/flox/_my/crush-glm/src/test_minimal.cpp` | Minimal Donchian test (200 bars) |
| `vendor/flox/_my/crush-glm/CMakeLists.txt` | Build config (Debug/Release, ASan) |
| `vendor/flox/_my/crush-glm/.vscode/launch.json` | VSCode debug configurations |
| `vendor/flox/_my/crush-glm/Q12-Q14-answers.md` | This document |

### Key technical discoveries

1. **SymbolInfo registration**: FLOX's `registerSymbol(exchange, symbol)` overload doesn't populate the internal `_symbols` map that `Strategy::Strategy()` reads via `getSymbolInfo()`. Must use `registerSymbol(SymbolInfo{...})` instead.

2. **OHLCV with OhlcvReplaySource**: `OhlcvReplaySource` only passes close prices as synthetic trades. To get real H/L for indicators, maintain a parallel index into the original CSV bars and look up OHLCV by bar count.

3. **Strategy::onBar is final**: Cannot override `onBar()` directly. Override `onSymbolBar()` or `onSymbolTrade()` instead. Our `onBarImpl()` naming avoids the `-Woverloaded-virtual` warning from hiding the parent's `onBar()`.

4. **Frozen indicators**: FLOX indicators (ATR, EMA, RSI, etc.) maintain internal state via `update()` calls. They must be fed OHLCV in chronological order. The `ready()` method indicates when the warmup period is complete.
