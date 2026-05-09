"""
Parallel grid search wrapper for FLOX.

Since FLOX's C++ runLocal() is currently sequential despite computing numThreads,
this script provides two approaches:

1. ThreadPoolExecutor  — best when using pybind11 (FLOX releases GIL)
2. ProcessPoolExecutor — for pure Python or when GIL is an issue

Usage:
    python3 grid_search_parallel.py --csv ../data/BTC_4H.csv --workers 8
"""

import argparse
import csv
import time
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# CSV reader (no pandas dependency)
# ---------------------------------------------------------------------------
def read_csv_bars(filepath: str) -> list[dict[str, Any]]:
    bars = []
    with open(filepath) as f:
        reader = csv.DictReader(f)
        for row in reader:
            bars.append({
                "timestamp": float(row.get("timestamp", 0)),
                "open": float(row["open"]),
                "high": float(row["high"]),
                "low": float(row["low"]),
                "close": float(row["close"]),
                "volume": float(row.get("volume", 0)),
            })
    return bars


# ---------------------------------------------------------------------------
# Parameter grids for each strategy
# ---------------------------------------------------------------------------
@dataclass
class StrategyParams:
    strategy: str
    params: dict

    def __str__(self):
        ps = ",".join(f"{k}={v}" for k, v in self.params.items())
        return f"{self.strategy}({ps})"


def build_grid() -> list[StrategyParams]:
    grid = []

    # Donchian Breakout
    for period in [10, 20, 40]:
        for sl in [1.5, 2.0, 2.5]:
            grid.append(StrategyParams("donchian_breakout", {"period": period, "atr_sl": sl}))

    # Dual Momentum
    for lookback in [6, 12, 24]:
        for sma in [20, 50, 100]:
            grid.append(StrategyParams("dual_momentum", {"lookback": lookback, "sma": sma}))

    # EMA Crossover
    for fast in [8, 12, 20]:
        for slow in [21, 26, 50]:
            if fast < slow:
                grid.append(StrategyParams("ema_crossover", {"fast": fast, "slow": slow}))

    # Keltner Breakout
    for ema in [20, 50]:
        for atr in [10, 14]:
            for mult in [1.5, 2.0, 2.5]:
                grid.append(StrategyParams("keltner_breakout", {"ema": ema, "atr": atr, "mult": mult}))

    # Keltner Squeeze
    for bb in [20, 40]:
        for kema in [20, 30]:
            for katr in [10, 14]:
                grid.append(StrategyParams("keltner_squeeze", {"bb": bb, "kema": kema, "katr": katr}))

    # Supertrend
    for period in [10, 14, 20]:
        for mult in [2.0, 3.0, 4.0]:
            grid.append(StrategyParams("supertrend", {"period": period, "mult": mult}))

    # TSMOM
    for lookback in [6, 12, 24]:
        for vol in [20, 40]:
            grid.append(StrategyParams("tsmom", {"lookback": lookback, "vol": vol}))

    # RSI-2
    for rsi in [2, 3]:
        for sma in [100, 200]:
            grid.append(StrategyParams("rsi2", {"rsi": rsi, "sma": sma}))

    # RSI + BB Mean Reversion
    for bb in [20, 40]:
        for rsi in [14, 21]:
            grid.append(StrategyParams("rsi_bb_mr", {"bb": bb, "rsi": rsi}))

    return grid


# ---------------------------------------------------------------------------
# Single backtest runner
# ---------------------------------------------------------------------------
def run_one(params: StrategyParams, bars: list[dict]) -> dict:
    """
    Run a single backtest. In production, this calls into FLOX via pybind11:

        import flox
        result = flox.run_one(strategy=params.strategy, **params.params, data=bars)

    For now, this is a placeholder that computes a simple signal.
    """
    closes = [b["close"] for b in bars]
    highs = [b["high"] for b in bars]
    lows = [b["low"] for b in bars]

    p = params.params
    equity = 10000.0
    position = 0.0
    trades = 0
    returns = []

    # Simple placeholder: each strategy has its own signal logic
    # In production, delegate to C++ FLOX
    lookback = p.get("period", p.get("lookback", p.get("fast", 20)))

    for i in range(max(lookback, 1), len(closes)):
        # Very simplified: buy if close > max of last N, sell if < min
        window_high = max(closes[i - lookback:i])
        window_low = min(closes[i - lookback:i])

        if position == 0 and closes[i] > window_high:
            position = equity / closes[i]
            equity = 0
            entry_price = closes[i]
            trades += 1
        elif position > 0 and closes[i] < window_low:
            equity = position * closes[i]
            pnl = (closes[i] / entry_price - 1.0) * 100
            returns.append(pnl)
            position = 0
            trades += 1

    # Close any open position
    if position > 0:
        equity = position * closes[-1]

    final_return = (equity / 10000.0 - 1.0) * 100 if position == 0 else (equity / 10000.0 - 1.0) * 100

    return {
        "strategy": params.strategy,
        "params": str(params),
        "return_pct": final_return,
        "trades": trades,
        "sharpe": sum(returns) / (len(returns) ** 0.5) if returns else 0.0,
    }


# ---------------------------------------------------------------------------
# Parallel execution
# ---------------------------------------------------------------------------
def run_parallel_threaded(grid: list[StrategyParams], bars: list[dict], workers: int) -> list[dict]:
    """Best for pybind11 FLOX (releases GIL)."""
    results = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(run_one, p, bars): p for p in grid}
        for i, future in enumerate(as_completed(futures), 1):
            results.append(future.result())
            if i % 10 == 0:
                print(f"\rProgress: {i}/{len(grid)} ({i*100//len(grid)}%)", end="", flush=True)
    print()
    return results


def run_parallel_processes(grid: list[StrategyParams], bars: list[dict], workers: int) -> list[dict]:
    """For pure Python (GIL-bound) or heavy CPU work outside GIL release."""
    # Note: bars must be picklable for ProcessPoolExecutor
    # Pass file path instead if memory is a concern
    results = []
    with ProcessPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(run_one, p, bars): p for p in grid}
        for i, future in enumerate(as_completed(futures), 1):
            results.append(future.result())
            if i % 10 == 0:
                print(f"\rProgress: {i}/{len(grid)} ({i*100//len(grid)}%)", end="", flush=True)
    print()
    return results


def main():
    parser = argparse.ArgumentParser(description="FLOX Parallel Grid Search")
    parser.add_argument("--csv", required=True, help="Path to OHLCV CSV file")
    parser.add_argument("--workers", type=int, default=8, help="Number of workers")
    parser.add_argument("--mode", choices=["threads", "processes"], default="threads",
                        help="Threading mode (threads recommended for pybind11 FLOX)")
    args = parser.parse_args()

    print(f"Loading {args.csv}...")
    bars = read_csv_bars(args.csv)
    print(f"Loaded {len(bars)} bars")

    grid = build_grid()
    print(f"Grid: {len(grid)} combinations")
    print(f"Mode: {args.mode} with {args.workers} workers")

    t0 = time.time()
    if args.mode == "threads":
        results = run_parallel_threaded(grid, bars, args.workers)
    else:
        results = run_parallel_processes(grid, bars, args.workers)
    elapsed = time.time() - t0

    # Sort by Sharpe
    results.sort(key=lambda r: r["sharpe"], reverse=True)

    print(f"\n=== Top 20 Results (elapsed: {elapsed:.1f}s) ===")
    for i, r in enumerate(results[:20]):
        if r["trades"] < 3:
            continue
        print(f"{i+1}. {r['params']}")
        print(f"   Sharpe: {r['sharpe']:.2f} | Return: {r['return_pct']:.1f}% | Trades: {r['trades']}")

    # Export
    out_path = Path(args.csv).parent / "grid_results.csv"
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["strategy", "params", "return_pct", "trades", "sharpe"])
        writer.writeheader()
        writer.writerows(results)
    print(f"\nResults exported to {out_path}")


if __name__ == "__main__":
    main()
