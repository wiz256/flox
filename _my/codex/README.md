# Codex FLOX MTF Grid Backtest

This folder is intentionally outside normal FLOX source ownership. It is a
debuggable sandbox for:

- C++ multi-symbol, multi-timeframe bar strategies.
- Grid backtests across parameters, exit modes, strategies, and symbols.
- CSV and mmap bar inputs.
- `.floxlog` raw replay inputs, converted to MTF bars inside the backtest.
- Python data scripts for Binance USDT-M candles and parquet staging.

Build:

```bash
cd /Users/lex/WorkspaceTrading/Trader7-Kilo/vendor/flox/_my/codex
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Smoke test with synthetic data:

```bash
./build/codex_mtf_grid_backtest --mode smoke
```

Download Binance futures M1 candles and convert to mmap bars:

```bash
python scripts/binance_futures_klines.py \
  --symbols BTCUSDT,ETHUSDT \
  --interval 1m \
  --start 2024-01-01 \
  --out data/binance_csv

./build/codex_mtf_grid_backtest \
  --mode convert-csv \
  --csv data/binance_csv/BTCUSDT_1m.csv \
  --symbol BTCUSDT \
  --out-mmap data/mmap/BTCUSDT
```

Run a grid over one or many symbols:

```bash
./build/codex_mtf_grid_backtest \
  --mode grid \
  --input-kind mmap \
  --data data/mmap \
  --symbols BTCUSDT,ETHUSDT \
  --strategies ema_crossover,donchian,rsi2,rsi_bb_mr \
  --threads 8 \
  --out results.csv
```

For `.floxlog`:

```bash
./build/codex_mtf_grid_backtest \
  --mode grid \
  --input-kind floxlog \
  --data /path/to/floxlog_dir \
  --symbols BTCUSDT \
  --strategies ema_crossover,donchian
```

Parquet is handled as staging because this local FLOX C++ build does not link
Arrow. Use `scripts/parquet_to_csv.py`, then `--mode convert-csv`.
