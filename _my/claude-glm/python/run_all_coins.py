"""
Run crush_grid across all downloaded coins and aggregate results.
Identifies strategies that work across multiple coins (robust edges).
Now uses robust_score (sharpe * plateauRatio) for ranking.

Usage:
    python3 run_all_coins.py --interval 4h --min-trades 30
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
RESULTS_DIR = SCRIPT_DIR / "results"


def run_crush_grid(csv_path: str, threads: int = 8, min_trades: int = 30,
                   wfo: bool = True, top_k: int = 20) -> list[dict]:
    """Run crush_grid and parse CSV output from results/ folder."""
    cmd = [str(CRUSH_GRID), csv_path, "--threads", str(threads),
           "--min-trades", str(min_trades)]
    if wfo:
        cmd.extend(["--wfo", "--top-k", str(top_k)])
    else:
        cmd.append("--no-wfo")

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    if result.returncode != 0:
        print(f"  Error: {result.stderr[:200]}")
        return []

    # Extract coin name to find the results file
    coin = Path(csv_path).stem.split("_")[0]
    csv_out = RESULTS_DIR / f"{coin}USDT_grid_results.csv"
    if not csv_out.exists():
        csv_out = RESULTS_DIR / f"{coin}_grid_results.csv"
    if not csv_out.exists():
        return []

    results = []
    with open(csv_out) as f:
        reader = csv.DictReader(f)
        for row in reader:
            results.append(row)
    return results


def main():
    parser = argparse.ArgumentParser(description="Multi-coin grid search with robustness")
    parser.add_argument("--interval", default="4h")
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--min-trades", type=int, default=30, help="Minimum trades filter")
    parser.add_argument("--top-n", type=int, default=5, help="Top N per coin to aggregate")
    parser.add_argument("--no-wfo", action="store_true", help="Disable walk-forward")
    parser.add_argument("--wfo-top-k", type=int, default=20, help="Top-K candidates for WFO")
    args = parser.parse_args()

    csv_files = sorted(DATA_DIR.glob(f"*_{args.interval}.csv"))
    if not csv_files:
        print(f"No CSV files found in {DATA_DIR} matching *_{args.interval}.csv")
        print("Run: python3 python/download_data.py --interval 4h --days 2000")
        sys.exit(1)

    print(f"Running robust grid search on {len(csv_files)} coins, {args.interval} bars")
    print(f"Filter: min {args.min_trades} trades, top {args.top_n} per coin by robust_score")
    print(f"WFO: {'ON' if not args.no_wfo else 'OFF'}\n")

    # strategy -> [list of (coin, sharpe, return, trades, plateau, robust_score, wfo_pass)]
    strategy_results = defaultdict(list)

    for csv_file in csv_files:
        symbol = csv_file.stem.split("_")[0]
        print(f"  {symbol}...", end="", flush=True)

        results = run_crush_grid(str(csv_file), args.threads, args.min_trades,
                                 wfo=not args.no_wfo, top_k=args.wfo_top_k)
        if not results:
            print(" no results")
            continue

        # Parse and sort by robust_score
        filtered = []
        for r in results:
            try:
                trades = int(float(r.get("total_trades", 0)))
                sharpe = float(r.get("sharpe_ratio", 0))
                ret = float(r.get("total_return", 0))
                dd_pct = float(r.get("max_drawdown_pct", 0))
                plateau = float(r.get("plateau_ratio", 0))
                robust = float(r.get("robust_score", 0))
                wfo_pass = float(r.get("wfo_pass_rate", -1))
                avg_nb_sharpe = float(r.get("avg_neighbor_sharpe", 0))
            except (ValueError, TypeError):
                continue
            if trades >= args.min_trades:
                filtered.append({
                    "symbol": symbol, "sharpe": sharpe, "return": ret,
                    "trades": trades, "dd_pct": dd_pct, "params": r.get("parameters", ""),
                    "plateau_ratio": plateau, "robust_score": robust,
                    "wfo_pass_rate": wfo_pass, "avg_neighbor_sharpe": avg_nb_sharpe
                })

        # Sort by robust_score (the key change from old sharpe-based ranking)
        filtered.sort(key=lambda x: x["robust_score"], reverse=True)
        top_n = filtered[:args.top_n]

        if top_n:
            best = top_n[0]
            wfo_str = f" WFO={best['wfo_pass_rate']:.0%}" if best['wfo_pass_rate'] >= 0 else ""
            print(f" {len(filtered)} valid, best RobustScore={best['robust_score']:.2f} "
                  f"(Sharpe={best['sharpe']:.2f}, Plateau={best['plateau_ratio']:.2f}{wfo_str})")
        else:
            print(f" {len(filtered)} valid (none with >={args.min_trades} trades)")

        for r in top_n:
            strategy_results[r["params"]].append(r)

    # Aggregate: find strategies that work on multiple coins
    print(f"\n=== Cross-Coin Robust Strategies ===")
    print(f"Strategies appearing in top-{args.top_n} by robust_score across multiple coins:\n")

    multi_coin = [(k, v) for k, v in strategy_results.items() if len(v) >= 2]
    multi_coin.sort(key=lambda x: (-len(x[1]), -sum(r["robust_score"] for r in x[1]) / len(x[1])))

    for params, results in multi_coin[:30]:
        coins = [r["symbol"] for r in results]
        avg_sharpe = sum(r["sharpe"] for r in results) / len(results)
        avg_robust = sum(r["robust_score"] for r in results) / len(results)
        avg_return = sum(r["return"] for r in results) / len(results)
        avg_plateau = sum(r["plateau_ratio"] for r in results) / len(results)
        wfo_rates = [r["wfo_pass_rate"] for r in results if r["wfo_pass_rate"] >= 0]
        avg_wfo = sum(wfo_rates) / len(wfo_rates) * 100 if wfo_rates else -1

        wfo_str = f" | WFO Pass: {avg_wfo:.0f}%" if avg_wfo >= 0 else ""
        print(f"  [{len(results)} coins] {params}")
        print(f"    Avg RobustScore: {avg_robust:.2f} | Avg Sharpe: {avg_sharpe:.2f} | Avg Plateau: {avg_plateau:.2f}{wfo_str}")
        print(f"    Avg Return: {avg_return:.1f}%")
        print(f"    Coins: {', '.join(coins)}")
        print()

    # Export
    RESULTS_DIR.mkdir(exist_ok=True)
    out_path = RESULTS_DIR / "cross_coin_results.csv"
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["params", "num_coins", "avg_robust_score", "avg_sharpe",
                         "avg_return", "avg_plateau_ratio", "avg_wfo_pass_rate", "coins"])
        for params, results in multi_coin:
            coins = ",".join(r["symbol"] for r in results)
            avg_rs = sum(r["robust_score"] for r in results) / len(results)
            avg_s = sum(r["sharpe"] for r in results) / len(results)
            avg_r = sum(r["return"] for r in results) / len(results)
            avg_p = sum(r["plateau_ratio"] for r in results) / len(results)
            wfo_rates = [r["wfo_pass_rate"] for r in results if r["wfo_pass_rate"] >= 0]
            avg_wfo = sum(wfo_rates) / len(wfo_rates) if wfo_rates else -1
            writer.writerow([params, len(results), f"{avg_rs:.4f}", f"{avg_s:.4f}",
                             f"{avg_r:.2f}", f"{avg_p:.4f}", f"{avg_wfo:.4f}", coins])
    print(f"Exported to {out_path}")

    # ===== Combined multi-coin CSVs =====
    # Merge all per-coin grid results into one file with coin column
    all_grid_rows = []
    all_wfo_rows = []
    for csv_file in sorted(RESULTS_DIR.glob("*_grid_results.csv")):
        coin = csv_file.stem.replace("_grid_results", "")
        with open(csv_file) as f:
            reader = csv.DictReader(f)
            for row in reader:
                row["coin"] = coin
                all_grid_rows.append(row)

    for csv_file in sorted(RESULTS_DIR.glob("*_wfo_results.csv")):
        coin = csv_file.stem.replace("_wfo_results", "")
        with open(csv_file) as f:
            reader = csv.DictReader(f)
            for row in reader:
                row["coin"] = coin
                all_wfo_rows.append(row)

    if all_grid_rows:
        # Read header from first file, add 'coin' column
        grid_cols = list(all_grid_rows[0].keys())
        # Move 'coin' to front
        if "coin" in grid_cols:
            grid_cols.remove("coin")
            grid_cols.insert(0, "coin")

        combined_grid = RESULTS_DIR / "combined_all_grid.csv"
        with open(combined_grid, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=grid_cols)
            writer.writeheader()
            writer.writerows(all_grid_rows)
        print(f"Combined grid: {len(all_grid_rows)} rows from {len(RESULTS_DIR.glob('*_grid_results.csv'))} coins → {combined_grid}")

    if all_wfo_rows:
        wfo_cols = list(all_wfo_rows[0].keys())
        if "coin" in wfo_cols:
            wfo_cols.remove("coin")
            wfo_cols.insert(0, "coin")

        combined_wfo = RESULTS_DIR / "combined_all_wfo.csv"
        with open(combined_wfo, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=wfo_cols)
            writer.writeheader()
            writer.writerows(all_wfo_rows)
        print(f"Combined WFO: {len(all_wfo_rows)} rows from {len(RESULTS_DIR.glob('*_wfo_results.csv'))} coins → {combined_wfo}")


if __name__ == "__main__":
    main()
