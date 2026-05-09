# FLOX Custom Strategies & Grid Backtests

C++ strategy implementations for FLOX engine with grid search parameter optimization.

## Directory Structure

```
vendor/flox/_my/crush-glm/
├── CMakeLists.txt              # Build configuration
├── README.md                   # This file
├── strategies/
│   ├── common.h                # Indicator math utilities (SMA, EMA, RSI, ATR, etc.)
│   ├── donchian_strategy.h     # Donchian channel breakout
│   ├── dual_momentum_strategy.h # Dual momentum (ROC + threshold)
│   ├── ema_crossover_strategy.h # EMA crossover with ADX filter
│   ├── keltner_breakout_strategy.h # Keltner channel breakout
│   ├── keltner_squeeze_strategy.h # Keltner squeeze (BB inside KC)
│   ├── supertrend_strategy.h   # Supertrend direction flip
│   ├── tsmom_strategy.h        # Time-series momentum with vol filter
│   ├── rsi2_strategy.h         # RSI(2) extreme oversold/overbought
│   └── rsi_bb_mr_strategy.h    # RSI + Bollinger Band mean reversion
├── grid_searches/
│   └── run_all_grids.cpp       # Grid search runner for all 9 strategies
├── scripts/
│   └── download_binance_data.py # Download/convert Binance Futures data
├── data/                       # Downloaded CSV data (gitignored)
│   ├── BTCUSDTUSDT_4h.csv      # ~11K bars from Binance USD-M
│   └── ...
└── crush-glm/
    └── Q1-Q13-flox-answers.md  # Comprehensive FLOX Q&A document
```

## Build

```bash
cd vendor/flox
cmake -B build -DFLOX_ENABLE_BACKTEST=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target run_all_grids
```

## Run Grid Searches

```bash
# All strategies on BTCUSDT 4h (default)
./build/run_all_grids

# Specific symbol and timeframe
./build/run_all_grids ETHUSDTUSDT 1h
```

Results are saved to `vendor/flox/_my/data/results_{strategy}_grid.csv`.

## Download Data

```bash
# Convert existing parquet data + download fresh from Binance
python vendor/flox/_my/scripts/download_binance_data.py

# Convert only (from existing parquet at /Users/lex/WorkspaceTrading/_data/)
python vendor/flox/_my/scripts/download_binance_data.py --convert-only

# Specific symbols and timeframes
python vendor/flox/_my/scripts/download_binance_data.py \
    --symbols BTCUSDT ETHUSDT SOLUSDT \
    --timeframes 1h 4h 1d
```

## Strategies

| Strategy | Type | Grid Size | Key Parameters |
|----------|------|-----------|----------------|
| **Donchian** | Trend following | 120 | window (8), atr_window (3), sl_mult (5) |
| **Dual Momentum** | Momentum | 40 | lookback (8), threshold (5) |
| **EMA Crossover** | Trend following | 168 | fast (7), slow (6), adx_threshold (4) |
| **Keltner Breakout** | Trend following | 60 | ema_window (5), atr_window (3), atr_mult (4) |
| **Keltner Squeeze** | Volatility breakout | 243 | ema_window (3), atr_window (3), kc_mult (3), bb_window (3), bb_std (3) |
| **Supertrend** | Trend following | 20 | atr_window (4), atr_mult (5) |
| **TSMOM** | Momentum | 120 | lookback (6), vol_window (4), vol_target (5) |
| **RSI2** | Mean reversion | 256 | rsi_window (4), oversold (4), overbought (4), sma (4) |
| **RSI BB MR** | Mean reversion | 576 | rsi_window (4), oversold (4), overbought (4), bb_window (3), bb_std (3) |

## Strategy Details

### Donchian
Channel breakout: enter long when close exceeds the highest close of the past `window` bars. Exit when close drops below the lowest close. Classic turtle-trading system.

### Dual Momentum
Rate of change (ROC) crossing a threshold. Enters when momentum crosses above threshold (long) or below negative threshold (short). Filters regime by requiring momentum confirmation.

### EMA Crossover
Fast EMA crossing slow EMA with optional ADX trend filter. Only enters when ADX > threshold, confirming a trending market. Exits on opposite crossover regardless of ADX.

### Keltner Breakout
Close exceeds Keltner Channel upper band (EMA + ATR * multiplier). Uses previous bar's band to avoid lookahead. Exits when price returns to the KC midline.

### Keltner Squeeze
Detects when Bollinger Bands are inside Keltner Channels (low volatility compression). Enters on squeeze release when price breaks above BB upper (long) or below BB lower (short).

### Supertrend
Follows the Supertrend indicator direction flips. When direction changes from bearish to bullish, go long. When bullish to bearish, go short. Simple and effective trend following.

### TSMOM (Time-Series Momentum)
ROC > 0 with volatility filter. Only trades when realized volatility is below target threshold. Annualized vol computed from rolling std of returns.

### RSI2
Exploits the extreme oscillation of RSI(2). Buys when RSI(2) < oversold threshold in an uptrend (close > SMA). The 2-period lookback oscillates violently, creating frequent oversold readings in bull markets that snap back quickly.

### RSI BB MR (RSI + Bollinger Band Mean Reversion)
Double confirmation: RSI oversold AND price at/below lower Bollinger Band. Two independent signals agreeing = higher quality entries. Exits when price returns to BB midline.

## Limitations (Current Implementation)

1. **Close-only indicators**: Strategies track close prices only. For strategies like Donchian and Supertrend that conceptually use high/low, we use close as approximation. Upgrade to MmapBarStorage for full OHLCV support.

2. **No exit modes yet**: Currently strategies use signal-based exits only. Complex exit modes (ATR trail, Chandelier, bracket, BE trail) from the Trader7-Kilo pipeline need to be added as strategy variants.

3. **Fixed position size**: Uses 0.01 lots. ATR-based position sizing can be added per strategy.

## Upgrading to Full OHLCV

To use proper high/low for indicators (ATR, Donchian, Keltner):

1. Convert CSV to MmapBarStorage binary format
2. Use `BacktestRunner::run_bars()` instead of `run_csv()`
3. Use `onSymbolBar()` callback instead of `onSymbolTrade()`
4. Access full Bar struct with `bar.high`, `bar.low`, etc.
