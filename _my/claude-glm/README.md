# FLOX Strategy Grid Search

**Date**: 2026-05-10

## Directory Structure

```
claude-glm/
├── README.md                     # This file
├── CMakeLists.txt                # Build system
├── build.sh                      # Build script
├── flox-deep-qa.md               # Q1-Q13 answers + Events deep-dive
├── strategies/
│   └── strategies.h              # 9 C++ strategy implementations (header-only)
├── csv_bar_reader.h              # Minimal CSV reader for OHLCV data
├── grid_search_main.cpp          # Multi-threaded grid search executable
├── python/
│   ├── grid_search_parallel.py   # Python parallel grid search wrapper
│   ├── csv_to_binary.py          # CSV -> FLOX binary converter
│   └── download_data.py          # Download OHLCV from Binance/Bybit
└── data/                         # Downloaded data files
```

## Quick Start

### 1. Download data

```bash
cd vendor/flox/_my/claude-glm
python3 python/download_data.py --symbol BTCUSDT --interval 4h --days 2000
```

### 2. Build

```bash
chmod +x build.sh && ./build.sh
```

Or manually:

```bash
cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
```

### 3. Run grid search

```bash
# Uses all CPU cores
./build/crush_grid data/BTCUSDT_4h.csv

# Use specific thread count
./build/crush_grid data/BTCUSDT_4h.csv 8
```

### 4. Python parallel wrapper

```bash
# Thread mode (recommended with pybind11 FLOX)
python3 python/grid_search_parallel.py --csv data/BTCUSDT_4h.csv --workers 8 --mode threads

# Process mode
python3 python/grid_search_parallel.py --csv data/BTCUSDT_4h.csv --workers 8 --mode processes
```

### 5. Convert CSV to binary (optional, for faster loading)

```bash
python3 python/csv_to_binary.py data/BTCUSDT_4h.csv data/BTCUSDT_4h.bin
```

## Strategies

All 9 strategies use `onSymbolBar()` (fires on bar close, receives full OHLCV).

| # | Strategy | Entry | Exit | Key Indicators |
|---|----------|-------|------|----------------|
| 1 | Donchian Breakout | Close > upper channel | Close < lower / ATR SL | Donchian, ATR |
| 2 | Dual Momentum | Momentum > 0 & price > SMA | Momentum reversal / SMA cross | SMA, returns |
| 3 | EMA Crossover | Fast EMA > Slow EMA (golden cross) | Death cross / ATR SL | EMA, ATR |
| 4 | Keltner Breakout | Close > Keltner upper band | Mean-revert or stop | EMA, ATR, Keltner |
| 5 | Keltner Squeeze | BB outside Keltner then breakout | Return to mean / re-squeeze | BB, Keltner |
| 6 | Supertrend | Direction flip (red -> green) | Direction flip (green -> red) | ATR, Supertrend |
| 7 | TSMOM | Return > 0 (vol-adjusted size) | Sign flip | Returns, vol |
| 8 | RSI-2 | Above SMA + RSI < 10 | RSI > 50 | RSI(2), SMA(200) |
| 9 | RSI+BB MR | Below lower BB + RSI oversold | Return to mean | RSI, Bollinger |

## Key Design Decisions

1. **`onSymbolBar` not `onSymbolTrade`**: 4H bar strategies need OHLCV data, not tick-by-tick.
2. **Streaming indicators**: Custom streaming EMA/ATR/RSI (lightweight, O(1) per bar update).
3. **`BacktestRunner::runBars()`**: Feeds `BarEvent` objects directly (full OHLCV), not synthetic trades.
4. **Multi-threaded grid search**: Custom `runParallel()` with `std::atomic` work stealing (fixes FLOX's sequential `runLocal()`).
5. **CSV reader**: Minimal built-in parser, no external dependencies.

## Grid Search Configuration

Edit `grid_search_main.cpp` `CombinedGrid` constructor to change parameter ranges.

Current ranges generate ~150-200 combinations across all 9 strategies.

## Profiling

```bash
# macOS Instruments (Time Profiler)
instruments -t "Time Profiler" ./build/crush_grid data/BTCUSDT_4h.csv 8

# Or build with Tracy
cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DFLOX_ENABLE_TRACY=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Or basic perf on Linux
perf record -g ./build/crush_grid data/BTCUSDT_4h.csv 8
perf report
```
