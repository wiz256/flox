"""
Download historical OHLCV data from public crypto APIs.
Saves as CSV to vendor/flox/_my/claude-glm/data/

Supports:
  - Binance (via public API, no auth needed)
  - Bybit (via public API)

Usage:
    python3 download_data.py --symbol BTCUSDT --interval 4h --days 2000
    python3 download_data.py --symbol ETHUSDT --interval 4h --source bybit
"""

import argparse
import csv
import json
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

DATA_DIR = Path(__file__).parent.parent / "data"
DATA_DIR.mkdir(exist_ok=True)


def download_binance(symbol: str, interval: str, start_ms: int, end_ms: int) -> list:
    """Download from Binance public klines API."""
    all_bars = []
    url = "https://api.binance.com/api/v3/klines"

    current = start_ms
    while current < end_ms:
        params = f"?symbol={symbol}&interval={interval}&startTime={current}&endTime={end_ms}&limit=1000"
        req = urllib.request.Request(url + params)
        req.add_header("User-Agent", "FLOX-GridSearch/1.0")

        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = json.loads(resp.read().decode())
        except Exception as e:
            print(f"  Error: {e}, retrying in 5s...")
            time.sleep(5)
            continue

        if not data:
            break

        for k in data:
            all_bars.append({
                "timestamp": int(k[0]),  # already milliseconds
                "open": float(k[1]),
                "high": float(k[2]),
                "low": float(k[3]),
                "close": float(k[4]),
                "volume": float(k[5]),
            })

        current = int(data[-1][0]) + 1
        print(f"\r  Downloaded {len(all_bars)} bars...", end="", flush=True)
        time.sleep(0.2)  # rate limit

    print()
    return all_bars


def download_bybit(symbol: str, interval: str, start_ms: int, end_ms: int) -> list:
    """Download from Bybit public klines API."""
    all_bars = []
    url = "https://api.bybit.com/v5/market/kline"

    # Bybit returns newest first, so we paginate backwards
    end = end_ms
    while end > start_ms:
        params = f"?category=spot&symbol={symbol}&interval={interval}&start={start_ms}&end={end}&limit=200"
        req = urllib.request.Request(url + params)
        req.add_header("User-Agent", "FLOX-GridSearch/1.0")

        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = json.loads(resp.read().decode())
        except Exception as e:
            print(f"  Error: {e}, retrying in 5s...")
            time.sleep(5)
            continue

        if data.get("retCode") != 0 or not data.get("result", {}).get("list"):
            break

        for k in data["result"]["list"]:
            all_bars.append({
                "timestamp": int(k[0]),
                "open": float(k[1]),
                "high": float(k[2]),
                "low": float(k[3]),
                "close": float(k[4]),
                "volume": float(k[5]),
            })

        # Move end to before the oldest bar we got
        oldest_ts = min(int(k[0]) for k in data["result"]["list"])
        end = oldest_ts - 1
        print(f"\r  Downloaded {len(all_bars)} bars...", end="", flush=True)
        time.sleep(0.2)

    print()
    # Sort chronologically
    all_bars.sort(key=lambda b: b["timestamp"])
    return all_bars


def save_csv(bars: list, filepath: str):
    with open(filepath, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["timestamp", "open", "high", "low", "close", "volume"])
        writer.writeheader()
        writer.writerows(bars)


def main():
    parser = argparse.ArgumentParser(description="Download OHLCV data")
    parser.add_argument("--symbol", default="BTCUSDT", help="Trading pair")
    parser.add_argument("--interval", default="4h", help="Bar interval (1m, 5m, 15m, 1h, 4h, 1d)")
    parser.add_argument("--days", type=int, default=2000, help="How many days of history")
    parser.add_argument("--source", choices=["binance", "bybit"], default="binance", help="Data source")
    args = parser.parse_args()

    end_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
    start_ms = end_ms - args.days * 24 * 3600 * 1000

    print(f"Downloading {args.symbol} {args.interval} from {args.source} ({args.days} days)...")

    if args.source == "binance":
        bars = download_binance(args.symbol, args.interval, start_ms, end_ms)
    else:
        bars = download_bybit(args.symbol, args.interval, start_ms, end_ms)

    if not bars:
        print("No data downloaded!")
        sys.exit(1)

    filename = f"{args.symbol.replace('/', '_')}_{args.interval}.csv"
    filepath = DATA_DIR / filename
    save_csv(bars, filepath)

    print(f"Saved {len(bars)} bars to {filepath}")
    print(f"  First: {datetime.fromtimestamp(bars[0]['timestamp']/1000, tz=timezone.utc)}")
    print(f"  Last:  {datetime.fromtimestamp(bars[-1]['timestamp']/1000, tz=timezone.utc)}")


if __name__ == "__main__":
    main()
