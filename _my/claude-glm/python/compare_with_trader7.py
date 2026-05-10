"""
Compare C++ grid search results with Trader7 (quant_scout-7) Python results.
Cross-validates strategy performance between the two engines.

Usage:
    python3 compare_with_trader7.py --coin BTC
    python3 compare_with_trader7.py --all-coins
"""

import argparse
import csv
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.parent
RESULTS_DIR = SCRIPT_DIR / "results"
TRADER7_CSV = Path("/Users/lex/WorkspaceTrading/Trader7-Kilo/tmp/quant_scout-7/"
                    "results/discovery/combined_strategy_coin_analysis_20260501/"
                    "combined_all_ranked.csv")


# Map between our C++ strategy names and Trader7 Python strategy names
STRATEGY_MAP = {
    "bollinger_breakout": "bollinger_breakout",
    "donchian": "donchian",
    "dual_momentum": "dual_momentum",
    "ema_crossover": "ema_crossover",
    "keltner_breakout": "keltner_breakout",
    "keltner_squeeze": "keltner_squeeze",
    "macd": "macd",
    "rsi_bb_mr": "rsi_bb_mr",
    "rsi2": "rsi2",
    "supertrend": "supertrend",
    "trend_pullback": "trend_pullback",
    "tsmom": "tsmom",
    "vol_compression_breakout": "vol_compression_breakout",
}


def parse_cpp_params(params_str: str) -> tuple:
    """Parse 'strategy_name(key=val,...)' into (strategy_name, {key: val})."""
    if "(" not in params_str:
        return params_str, {}
    name, rest = params_str.split("(", 1)
    rest = rest.rstrip(")")
    params = {}
    for part in rest.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            params[k.strip()] = v.strip()
    return name, params


def load_trader7_results() -> list[dict]:
    """Load Trader7 combined results CSV."""
    if not TRADER7_CSV.exists():
        print(f"Trader7 results not found: {TRADER7_CSV}")
        return []

    results = []
    with open(TRADER7_CSV) as f:
        reader = csv.DictReader(f)
        for row in reader:
            results.append(row)
    return results


def load_cpp_results(coin: str) -> list[dict]:
    """Load our C++ grid search results for a specific coin."""
    csv_path = RESULTS_DIR / f"{coin}USDT_grid_results.csv"
    if not csv_path.exists():
        csv_path = RESULTS_DIR / f"{coin}_grid_results.csv"
    if not csv_path.exists():
        return []

    results = []
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            results.append(row)
    return results


def compare_coin(coin: str):
    """Compare C++ vs Trader7 results for a specific coin."""
    print(f"\n{'='*60}")
    print(f"Comparing results for {coin}")
    print(f"{'='*60}")

    trader7 = load_trader7_results()
    cpp = load_cpp_results(coin)

    if not trader7:
        print("  No Trader7 results found")
        return
    if not cpp:
        print(f"  No C++ results found for {coin}")
        return

    # Group Trader7 results by coin (T7 uses "BTC", we use "BTCUSDT")
    t7_by_coin = defaultdict(list)
    for row in trader7:
        t7_coin = row.get("coin", "").upper()
        # Match "BTC" to "BTCUSDT" or just "BTC"
        if t7_coin == coin or t7_coin == f"{coin}USDT":
            t7_by_coin[row["strategy"]].append(row)

    # Group C++ results by strategy
    cpp_by_strat = defaultdict(list)
    for row in cpp:
        name, params = parse_cpp_params(row.get("parameters", ""))
        cpp_by_strat[name].append(row)

    print(f"\n  C++ strategies with results: {len(cpp_by_strat)}")
    print(f"  Trader7 strategies for {coin}USDT: {len(t7_by_coin)}")

    # Compare per-strategy
    print(f"\n  {'Strategy':<25} | {'C++ Best Sharpe':>15} | {'T7 Best Sharpe':>15} | {'C++ #Results':>12} | {'T7 #Results':>11}")
    print(f"  {'-'*25}-+-{'-'*15}-+-{'-'*15}-+-{'-'*12}-+-{'-'*11}")

    for strat_name in sorted(set(list(cpp_by_strat.keys()) + list(t7_by_coin.keys()))):
        cpp_results = cpp_by_strat.get(strat_name, [])
        t7_results = t7_by_coin.get(strat_name, [])

        cpp_best_sharpe = 0.0
        if cpp_results:
            try:
                cpp_best_sharpe = max(float(r.get("sharpe_ratio", 0)) for r in cpp_results)
            except (ValueError, TypeError):
                pass

        t7_best_sharpe = 0.0
        if t7_results:
            try:
                t7_best_sharpe = max(float(r.get("best_sharpe", 0)) for r in t7_results)
            except (ValueError, TypeError):
                pass

        cpp_count = len(cpp_results)
        t7_count = len(t7_results)

        if cpp_count > 0 or t7_count > 0:
            print(f"  {strat_name:<25} | {cpp_best_sharpe:>15.2f} | {t7_best_sharpe:>15.2f} | {cpp_count:>12} | {t7_count:>11}")

    # Top 5 from each engine
    print(f"\n  Top 5 C++ strategies for {coin}:")
    cpp_sorted = sorted(cpp, key=lambda x: float(x.get("robust_score", 0)), reverse=True)[:5]
    for i, r in enumerate(cpp_sorted, 1):
        name, _ = parse_cpp_params(r.get("parameters", ""))
        sharpe = float(r.get("sharpe_ratio", 0))
        robust = float(r.get("robust_score", 0))
        plateau = float(r.get("plateau_ratio", 0))
        print(f"    {i}. {r.get('parameters', '')}")
        print(f"       Sharpe={sharpe:.2f} | RobustScore={robust:.2f} | Plateau={plateau:.2f}")

    t7_sorted = sorted(
        [r for r in trader7 if r.get("coin", "").upper() == f"{coin}USDT"],
        key=lambda x: float(x.get("promise_score", 0)), reverse=True
    )[:5]
    if t7_sorted:
        print(f"\n  Top 5 Trader7 strategies for {coin}:")
        for i, r in enumerate(t7_sorted, 1):
            print(f"    {i}. {r['strategy']}/{r['coin']} (period={r.get('best_period', '?')})")
            print(f"       Promise={r.get('promise_score', '0')} | Sharpe={r.get('best_sharpe', '0')[:8]} | "
                  f"Trades={r.get('best_n_trades', '?')}")


def main():
    parser = argparse.ArgumentParser(description="Compare C++ vs Trader7 results")
    parser.add_argument("--coin", help="Single coin to compare (e.g., BTC, ETH)")
    parser.add_argument("--all-coins", action="store_true", help="Compare all available coins")
    args = parser.parse_args()

    if args.all_coins:
        # Find all coins with C++ results
        coins = set()
        for f in RESULTS_DIR.glob("*_grid_results.csv"):
            coin = f.stem.replace("_grid_results", "").replace("USDT", "")
            coins.add(coin)
        for coin in sorted(coins):
            compare_coin(coin)
    elif args.coin:
        compare_coin(args.coin.upper())
    else:
        # Default: compare BTC and ETH
        for coin in ["BTC", "ETH", "SOL"]:
            compare_coin(coin)


if __name__ == "__main__":
    main()
