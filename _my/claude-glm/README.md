# FLOX Strategy Grid Search — 13 Strategies × 40 Coins

**Date**: 2026-05-10

## Directory Structure

```
claude-glm/
├── README.md
├── CMakeLists.txt
├── build.sh
├── flox-deep-qa.md
├── strategies/
│   └── strategies.h          # 13 C++ strategies, streaming indicators
├── csv_bar_reader.h           # Minimal CSV parser
├── grid_search_main.cpp       # Multi-threaded grid search (1594 combos)
├── python/
│   ├── grid_search_parallel.py  # Python parallel wrapper
│   ├── csv_to_binary.py         # CSV → binary converter
│   ├── download_data.py         # Download 40 Binance Futures coins
│   └── run_all_coins.py         # Multi-coin aggregation + robustness
└── data/                        # Downloaded OHLCV CSV files
```

## Quick Start

```bash
cd vendor/flox/_my/claude-glm

# 1. Download data (40 coins, ~2000 days each)
python3 python/download_data.py --interval 4h --days 2000

# 2. Build
./build.sh

# 3. Single-coin grid search (8 threads)
./build/crush_grid data/BTCUSDT_4h.csv 8

# 4. Multi-coin aggregation (finds strategies working across many coins)
python3 python/run_all_coins.py --interval 4h --min-trades 30
```

## 13 Strategies with Exact Python Grid Params

| # | Strategy | Grid Size | Key Params |
|---|----------|-----------|------------|
| 1 | bollinger_breakout | 48 | bb=[10,20,30,50] std=[1.5-3.0] vol_mult=[0,1.3,1.5] |
| 2 | donchian | 120 | channel=[10-55] exit=[5-20] vol_mult=[0-2.0] |
| 3 | dual_momentum | 36 | lookback=[20-120] threshold=[0-0.05] smooth=[1,3,5] |
| 4 | ema_crossover | 88 | fast=[5-34] slow=[21-144] adx_min=[0-25] |
| 5 | keltner_breakout | 504 | ema=[10-80] atr=[10-30] mult=[2.0-3.5] atr_pct=[0-0.008] |
| 6 | keltner_squeeze | 81 | ema=[10-30] kelt_mult=[1.5-2.5] bb=[10-30] bb_std=[1.5-2.5] |
| 7 | macd | 81 | fast=[8-16] slow=[21-34] signal=[7-12] trend=[0-200] |
| 8 | rsi_bb_mr | 243 | rsi=[7-21] rsi_low=[20-30] rsi_high=[70-80] bb=[15-30] std=[1.5-2.5] |
| 9 | rsi2 | 54 | rsi=[2,3] entry_low=[5-15] entry_high=[85-95] trend=[50-200] |
| 10 | supertrend | 60 | atr=[7-20] mult=[2.0-4.0] adx_min=[0-25] |
| 11 | trend_pullback | 108 | fast=[10-30] slow=[50-200] rsi_pb=[35-45] atr_pct=[0-0.008] |
| 12 | tsmom | 72 | lookback=[20-160] smooth=[1-10] atr_pct=[0-0.008] |
| 13 | vol_compression_breakout | 99 | range=[20-60] compression=[60-150] pct=[0.2-0.4] vol=[0-1.5] |
| | **Total** | **1594** | |

## Finding Robust Edges (Not Just Sharpe Spikes)

The grid search alone finds the highest Sharpe, but that's often overfit.
Use this pipeline:

1. **crush_grid** → 1594 combos × 8 threads (DONE)
2. **Min trade filter** → require 20+ trades (eliminates lucky 5-trade runs)
3. **Multi-coin validation** → `run_all_coins.py` finds strategies working across 10+ coins
4. **Walk-forward** → use `flox_py.WalkForwardRunner` on top-K candidates
5. **White's Reality Check** → `flox_py.whites_reality_check` on all 1594 returns
6. **Neighborhood check** → ±1 param step must also be profitable

### What FLOX already provides:
- `WalkForwardRunner` — train/test split per fold
- `whites_reality_check()` — bootstrap test for best-of-N significance
- `OptimizationStatistics::bootstrapCI` — confidence intervals
- `OptimizationStatistics::permutationTest` — group comparison

### What needs custom implementation:
- Surface plateau detection (from `surface.py`)
- PBO-CSCV (from `robustness.py`)
- Multi-coin aggregation (in `run_all_coins.py`)
- Trade diagnostics (MAE/MFE/R-multiple)

## Profiling

```bash
# macOS Instruments
instruments -t "Time Profiler" ./build/crush_grid data/BTCUSDT_4h.csv 8

# Tracy
cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DFLOX_ENABLE_TRACY=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Linux perf
perf record -g ./build/crush_grid data/BTCUSDT_4h.csv 8
perf report
```
