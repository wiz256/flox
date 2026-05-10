"""
Run crush_grid across all downloaded coins and aggregate results.
Identifies strategies that work across multiple coins (robust edges).

Usage:
    python3 run_all_coins.py --interval 4h --min-trades 20
"""

import argparse
import csv
import os
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.parent
CRUSH_GRID = SCRIPT_DIR / "build" / "crush_grid"
DATA_DIR = SCRIPT_DIR / "data"


def run_crush_grid(csv_path: str, threads: int = 8) -> list[dict]:
    """Run crush_grid and parse CSV output."""
    result = subprocess.run(
        [str(CRUSH_GRID), csv_path, str(threads)],
        capture_output=True, text=True, timeout=120
    )
    if result.returncode != 0:
        print(f"  Error: {result.stderr[:200]}")
        return []

    csv_path_out = "grid_search_results.csv"
    if not os.path.exists(csv_path_out):
        return []

    results = []
    with open(csv_path_out) as f:
        reader = csv.DictReader(f)
        for row in reader:
            results.append(row)
    return results


def main():
    parser = argparse.ArgumentParser(description="Multi-coin grid search")
    parser.add_argument("--interval", default="4h")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--min-trades", type=int, default=20, help="Minimum trades filter")
    parser.add_argument("--top-n", type=int, default=5, help="Top N per coin to aggregate")
    args = parser.parse_args()

    csv_files = sorted(DATA_DIR.glob(f"*_{args.interval}.csv"))
    if not csv_files:
        print(f"No CSV files found in {DATA_DIR} matching *_{args.interval}.csv")
        print("Run: python3 python/download_data.py --interval 4h --days 2000")
        sys.exit(1)

    print(f"Running grid search on {len(csv_files)} coins, {args.interval} bars")
    print(f"Filter: min {args.min_trades} trades, top {args.top_n} per coin\n")

    # strategy -> [list of (coin, sharpe, return, trades)]
    strategy_results = defaultdict(list)
    coin_count = 0

    for csv_file in csv_files:
        symbol = csv_file.stem.split("_")[0]
        print(f"  {symbol}...", end="", flush=True)

        results = run_crush_grid(str(csv_file), args.threads)
        if not results:
            print(" no results")
            continue

        # Filter by min trades and get top N
        filtered = []
        for r in results:
            try:
                trades = int(float(r.get("total_trades", 0)))
                sharpe = float(r.get("sharpe_ratio", 0))
                ret = float(r.get("total_return", 0))
                dd = float(r.get("max_drawdown", 0))
            except (ValueError, TypeError):
                continue
            if trades >= args.min_trades:
                filtered.append({"symbol": symbol, "sharpe": sharpe, "return": ret,
                                 "trades": trades, "dd": dd, "params": r.get("parameters", "")})

        filtered.sort(key=lambda x: x["sharpe"], reverse=True)
        top_n = filtered[:args.top_n]

        if top_n:
            print(f" {len(filtered)} valid, best Sharpe={top_n[0]['sharpe']:.2f}")
        else:
            print(f" {len(filtered)} valid (none with >={args.min_trades} trades)")

        for r in top_n:
            strategy_results[r["params"]].append(r)
        coin_count += 1

    # Aggregate: find strategies that work on multiple coins
    print(f"\n=== Cross-Coin Robust Strategies ===")
    print(f"Strategies appearing in top-{args.top_n} across multiple coins:\n")

    multi_coin = [(k, v) for k, v in strategy_results.items() if len(v) >= 2]
    multi_coin.sort(key=lambda x: len(x[1]), reverse=True)

    for params, results in multi_coin[:30]:
        coins = [r["symbol"] for r in results]
        avg_sharpe = sum(r["sharpe"] for r in results) / len(results)
        avg_return = sum(r["return"] for r in results) / len(results)
        print(f"  [{len(results)} coins] {params}")
        print(f"    Avg Sharpe: {avg_sharpe:.2f} | Avg Return: {avg_return:.1f}%")
        print(f"    Coins: {', '.join(coins)}")
        print()

    # Export
    out_path = DATA_DIR / "cross_coin_results.csv"
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["params", "num_coins", "avg_sharpe", "avg_return", "coins"])
        for params, results in multi_coin:
            coins = ",".join(r["symbol"] for r in results)
            avg_s = sum(r["sharpe"] for r in results) / len(results)
            avg_r = sum(r["return"] for r in results) / len(results)
            writer.writerow([params, len(results), f"{avg_s:.4f}", f"{avg_r:.2f}", coins])
    print(f"Exported to {out_path}")


if __name__ == "__main__":
    main()
