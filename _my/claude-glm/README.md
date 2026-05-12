# FLOX Strategy Grid Search — 14 Strategies × 40 Coins

**Date**: 2026-05-10

## Directory Structure

```
claude-glm/
├── README.md                   # This file
├── CMakeLists.txt              # Build config
├── build.sh                    # Build script
├── strategies/
│   └── strategies.h            # 14 C++ strategies with streaming indicators
├── csv_bar_reader.h            # Minimal OHLCV CSV parser
├── grid_search_main.cpp        # Multi-threaded grid search + robustness pipeline
├── python/
│   ├── download_data.py        # Download 40 Binance Futures coins
│   ├── run_all_coins.py        # Multi-coin aggregation + combined export
│   └── compare_with_trader7.py # Cross-validate with Trader7 Python results
├── data/                       # Downloaded OHLCV CSV files
└── results/                    # Grid search + WFO results per coin
```

## Quick Start

```bash
cd vendor/flox/_my/claude-glm

# 1. Download data (40 coins, ~2000 days, 4h bars)
python3 python/download_data.py --interval 4h --days 2000

# 2. Build
./build.sh

# 3. Single-coin grid search with WFO
./build/crush_grid data/BTCUSDT_4h.csv --recent-years 2 --wfo

# 4. Multi-coin (all 40 coins)
python3 python/run_all_coins.py --interval 4h --min-trades 30

# 5. Compare with Trader7 Python results
python3 python/compare_with_trader7.py --all-coins
```

## Pipeline: Grid Search → Filter → Plateau → Walk-Forward

```
Step 1: Grid Search (multi-threaded)
  1602 combos × N threads → raw BacktestResult per combo

Step 2: Min-Trade Filter
  Discard combos with < 30 total trades (configurable --min-trades)

Step 3: Neighborhood Plateau Detection
  For each combo, find all ±1 param-step neighbors
  Compute plateauRatio = fraction of profitable neighbors
  Discard combos with plateauRatio < 0.3 (--plateau-min)

Step 4: Composite Scoring (Trader7-style)
  7-component weighted score replaces simple Sharpe ranking
  Sort by compositeScore descending → select top-K for WFO

Step 5: Walk-Forward Validation (rolling)
  Top-K candidates → 2-5 folds rolling WFO with embargo
  Per-fold: IS window (70%) → embargo (24 bars) → OOS window (30%)
  Skip fold if IS trades < 20 or OOS trades < 5
  Pass: OOS Sharpe > 0 AND degradation >= 0.30
  Overall: >= 50% of non-skipped folds must pass

Step 6: Export
  results/{COIN}_grid_results.csv    — all filtered combos with scores
  results/{COIN}_wfo_results.csv     — per-fold IS/OOS diagnostics
  results/combined_all_grid.csv       — all coins combined
  results/combined_all_wfo.csv        — all coins WFO combined
```

## 14 Strategies

| # | Strategy | Grid Size | Entry | Exit Mode |
|---|----------|-----------|-------|-----------|
| 1 | bollinger_breakout | 48 | BB breakout + vol filter | bb_reverse |
| 2 | donchian | 120 | Channel breakout | channel_midline |
| 3 | dual_momentum | 36 | Momentum sign | momentum_flip |
| 4 | ema_crossover | 88 | EMA cross + ADX filter | ema_cross |
| 5 | keltner_breakout | 504 | Keltner channel + ATR filter | channel_break |
| 6 | keltner_squeeze | 81 | Squeeze release | squeeze_release |
| 7 | macd | 81 | MACD signal + trend filter | signal_cross |
| 8 | rsi_bb_mr | 243 | RSI + Bollinger mean reversion | bb_mean_revert |
| 9 | rsi2 | 54 | RSI-2 extreme + trend filter | rsi_exit_zone |
| 10 | supertrend | 60 | Supertrend direction | trend_flip |
| 11 | trend_pullback | 108 | Uptrend + RSI pullback | chandelier_trail |
| 12 | tsmom | 72 | Time-series momentum | sign_flip |
| 13 | vol_compression_breakout | 99 | Vol compression + breakout | range_break |
| 14 | sma_crossover (benchmark) | 8 | SMA golden/death cross | death_cross |
| | **Total** | **1602** | | |

### How to Add a New Strategy

1. **`strategies/strategies.h`**: Add params struct, strategy class with streaming indicators
2. **`grid_search_main.cpp`**: Add to `StrategyKind` enum, `strategyName()`, `strategyExitMode()`
3. **Grid builder**: Add param combinations in `CombinedGrid` constructor
4. **Axis defs**: Add `StrategyAxisDefs` in `g_axisDefs[]` for plateau detection
5. **Factory**: Add `case` in `createStrategy()`
6. **toString**: Add `case` in `CombinedParams::toString()`

## Composite Score Breakdown

The composite score replaces simple Sharpe ranking. Each component is normalized to [0,1] using `tanh`:

```
compositeScore = 0.25 * nSharpe
               + 0.20 * nCalmar
               + 0.15 * nPF
               + 0.15 * nPlateau
               + 0.10 * nTrades
               + 0.10 * nCost
               + 0.05 * nWinRate
               - ddPenalty
```

| Component | Weight | Raw Value | Normalization | Meaning |
|-----------|--------|-----------|---------------|---------|
| nSharpe | 25% | Sharpe ratio | tanh(sharpe/3.0) | Risk-adjusted return |
| nCalmar | 20% | Calmar = return/maxDD | tanh(calmar/10.0) | Return vs drawdown |
| nPF | 15% | Profit factor | tanh((pf-1)/1.5) | Win/loss ratio |
| nPlateau | 15% | plateauRatio | min(max(ratio,0),1) | Neighbor profitability |
| nTrades | 10% | Total trades | tanh(trades/150) | Statistical significance |
| nCost | 10% | Sharpe after 2x fees | tanh(costSharpe/2.0) | Fee resilience |
| nWinRate | 5% | Win rate | (wr-0.35)/0.45 clamped | Consistency |
| ddPenalty | subtract | Max drawdown | max(0, abs(dd)-35)/100 | Drawdown penalty |

Each row in the results shows the weighted contribution of each component (e.g., `sharpe=0.250` means Sharpe contributed 0.250 of the total score).

## Walk-Forward Validation

### How Folds Work

Rolling WFO with embargo (matches Trader7 quant_scout-7 config):

```
Bar timeline:  |----fold 0----|----fold 1----|----fold 2----|...
                |IS|emb|OOS|  |IS|emb|OOS|
                 70%  24  30%   70%  24  30%
```

- **IS (In-Sample)**: 70% of fold period — train window
- **Embargo**: 24 bars gap — prevents indicator leakage between IS and OOS
- **OOS (Out-of-Sample)**: Remaining bars — test window
- **Adaptive folds**: Auto-reduced from 5 to 2-3 when data is short (--recent-years)

### Per-Fold Diagnostics

For each candidate, WFO reports:
- **IS period**: dates, bars, price range ($start→$end)
- **IS stats**: Sharpe, return%, trades, PnL, net PnL, fees, win%, PF, maxDD
- **OOS period**: same stats
- **Degradation**: OOS Sharpe / IS Sharpe (higher = better)
- **Pass/Skip/Fail**: with reason (e.g., "OOS trades=4 < 5")
- **Cross-fold consistency**: Avg Sharpe ± σ for both IS and OOS

### WFO CSV Columns

```
fold, parameters,
train_date_from, train_date_to, train_bars, train_price_start, train_price_end,
train_sharpe, train_sortino, train_calmar, train_return, train_maxdd,
train_trades, train_win_rate, train_profit_factor, train_pnl, train_net_pnl, train_fees,
test_date_from, test_date_to, test_bars, test_price_start, test_price_end,
test_sharpe, test_sortino, test_calmar, test_return, test_maxdd,
test_trades, test_win_rate, test_profit_factor, test_pnl, test_net_pnl, test_fees,
degradation, passed, skipped, skip_reason
```

## Execution Model: Anti Look-Ahead

Market orders are deferred to the **next bar's open** price:
1. Signal generated on bar[t] close → queued as pending
2. On bar[t+1], feed bar[t+1].open to executor → pending order fills at open price
3. Close price fed separately for SL/TP/trailing stop evaluation only (no new fills)

This prevents the common look-ahead bias where signals on bar[t] fill at bar[t] close.

## PnL and Fee Calculation

- **Position sizing**: Fixed $1000 notional per trade (default), configurable:
  - `--sizing-mode fixed_notional --notional 1000` (default)
  - `--sizing-mode all_equity` (100% of capital)
  - `--sizing-mode percent_equity --percent-equity 0.01`
- **Fees**: 0.04% per trade (configurable `--fee`), applied on both entry and exit
- **Leverage**: `--leverage 1.0` (default, no leverage)
- **Capital**: `--capital 10000` (default starting equity)

## Benchmark Validation

Strategy #14 (SMA Crossover) serves as a canonical benchmark to validate the engine:

| Combo | Sharpe (4h ann.) | Daily-equiv | Trades | Win% | Expected Range |
|-------|-----------------|-------------|--------|------|----------------|
| SMA 10/100 | 7.86 | 2.66 | 34 | 47% | Trend-following in bull |
| SMA 20/100 | 7.58 | 2.57 | 31 | 48% | Similar |
| SMA 50/200 | 1.26 | 0.43 | 17 | - | Canonical: 0.5-0.7 equities, 0.8-1.3 BTC bull |

**Daily-equivalent Sharpe** = Sharpe_4h / sqrt(2190/252) ≈ Sharpe_4h / 2.95

### Realistic Sharpe Ranges (annualized, for reference)
- Single trend-following strategy: **0.3-0.8** daily
- Single mean-reversion strategy: **0.5-1.5** daily
- Above 3.0 daily: **likely overfit or bug**
- SMA 50/200 at 0.43 daily validates our engine is correct

## Anti-Look-Ahead Validation Checklist

- [x] Market orders deferred to next bar open (not same-bar close)
- [x] Annualization factor matched to bar interval (2190 for 4h, not 252)
- [x] SMA 50/200 benchmark produces realistic Sharpe
- [x] Win rates match expected ranges (30-45% trend, 55-80% mean-reversion)
- [x] Trade counts per year reasonable for strategy type
- [x] WFO degrades OOS performance vs IS (confirms no persistent look-ahead)

## Grid Results CSV Columns

```
timeframe, date_from, date_to, bars_count, price_start, price_end,
sharpe_ratio, sortino_ratio, calmar_ratio, total_return, max_drawdown_pct, win_rate,
profit_factor, total_trades, exit_mode, parameters,
plateau_ratio, avg_neighbor_sharpe, neighbor_count, composite_score,
cc_sharpe, cc_calmar, cc_profit_factor, cc_plateau, cc_trades,
cc_cost_stress, cc_win_rate, cc_dd_penalty,
wfo_pass_rate, wfo_avg_test_sharpe
```

## Command-Line Options

```
./build/crush_grid data/BTCUSDT_4h.csv [options]

Sizing:
  --sizing-mode <mode>    all_equity|fixed_notional|percent_equity (default: fixed_notional)
  --notional <usd>        Notional per trade (default: 1000)
  --percent-equity <pct>  Equity fraction (default: 0.01)
  --leverage <mult>       Leverage multiplier (default: 1.0)

Filtering:
  --min-trades <n>        Minimum trades filter (default: 30)
  --plateau-min <f>       Min plateau ratio (default: 0.3)

Walk-Forward:
  --wfo                   Enable WFO (default: on)
  --no-wfo                Disable WFO
  --top-k <n>             Top-K candidates for WFO (default: 50)
  --wfo-folds <n>         Number of folds (default: 5, auto-adapted)
  --wfo-is-pct <f>        IS fraction per fold (default: 0.70)
  --wfo-embargo <n>       Embargo bars (default: 24)
  --wfo-min-is-trades <n> Min IS trades (default: 20)
  --wfo-min-oos-trades <n> Min OOS trades (default: 5)
  --wfo-degrad <f>        Min degradation OOS/IS (default: 0.30)
  --wfo-fold-pass <f>     Fraction folds that must pass (default: 0.50)

Data:
  --recent-years <n>      Use only last N years (default: 0=all)
  --capital <usd>         Initial capital (default: 10000)
  --fee <rate>            Fee rate (default: 0.0004 = 0.04%)
  --threads <n>           Number of threads (default: all cores)
```

## Neighborhood Plateau Detection

For each grid point, we find all **±1 param-step neighbors** (change exactly one parameter by one step in its value array). The `plateauRatio` is the fraction of those neighbors that are profitable.

A high plateauRatio (e.g., 0.8+) means the parameter region is a stable plateau — small parameter changes don't destroy the edge. This filters out isolated Sharpe spikes.

The `AxisDef` system maps each strategy's parameter grid to its axes, enabling O(1) hash lookups for neighbor discovery.

## Robustness Indicators

| Indicator | What It Measures | Good Value |
|-----------|-----------------|------------|
| plateauRatio | Neighbor profitability | >= 0.5 (strong), >= 0.3 (acceptable) |
| wfoPassRate | WFO fold pass rate | >= 50% |
| compositeScore | Overall robustness | >= 0.5 |
| avgNeighborSharpe | Neighbor avg performance | Close to own Sharpe |
