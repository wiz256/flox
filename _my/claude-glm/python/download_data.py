"""
Download historical OHLCV data from Binance Futures for top 40 coins.
Saves as CSV to vendor/flox/_my/claude-glm/data/

Usage:
    python3 download_data.py --interval 4h --days 2000
    python3 download_data.py --symbols BTCUSDT ETHUSDT --interval 4h
"""

import argparse
import csv
import json
import sys
import time
import urllib.request
from pathlib import Path

DATA_DIR = Path(__file__).parent.parent / "data"
DATA_DIR.mkdir(exist_ok=True)

# Top 40 Binance Futures by volume
DEFAULT_SYMBOLS = [
    "AAVEUSDT", "ADAUSDT", "APTUSDT", "ARBUSDT", "AVAXUSDT",
    "BCHUSDT", "BNBUSDT", "BTCUSDT", "CRVUSDT", "DOGEUSDT",
    "DOTUSDT", "ENAUSDT", "ETHUSDT", "FETUSDT", "FILUSDT",
    "HBARUSDT", "HYPEUSDT", "INJUSDT", "LDOUSDT", "LINKUSDT",
    "LTCUSDT", "NEARUSDT", "OPUSDT", "ORDIUSDT", "PENGUUSDT",
    "RUNEUSDT", "SOLUSDT", "SUIUSDT", "TAOUSDT", "TIAUSDT",
    "TONUSDT", "TRXUSDT", "UNIUSDT", "VIRTUALUSDT", "WIFUSDT",
    "WLDUSDT", "XLMUSDT", "XMRUSDT", "XRPUSDT", "ZECUSDT",
]


def download_futures_klines(symbol: str, interval: str, start_ms: int, end_ms: int) -> list:
    """Download from Binance Futures klines API."""
    all_bars = []
    url = "https://fapi.binance.com/fapi/v1/klines"
    current = start_ms

    while current < end_ms:
        params = f"?symbol={symbol}&interval={interval}&startTime={current}&endTime={end_ms}&limit=1500"
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
                "timestamp": int(k[0]),
                "open": float(k[1]),
                "high": float(k[2]),
                "low": float(k[3]),
                "close": float(k[4]),
                "volume": float(k[5]),
            })

        current = int(data[-1][0]) + 1
        time.sleep(0.15)  # rate limit

    return all_bars


def download_spot_klines(symbol: str, interval: str, start_ms: int, end_ms: int) -> list:
    """Download from Binance Spot klines API (fallback)."""
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
                "timestamp": int(k[0]),
                "open": float(k[1]),
                "high": float(k[2]),
                "low": float(k[3]),
                "close": float(k[4]),
                "volume": float(k[5]),
            })

        current = int(data[-1][0]) + 1
        time.sleep(0.2)

    return all_bars


def save_csv(bars: list, filepath: str):
    with open(filepath, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["timestamp", "open", "high", "low", "close", "volume"])
        writer.writeheader()
        writer.writerows(bars)


def main():
    parser = argparse.ArgumentParser(description="Download OHLCV data from Binance")
    parser.add_argument("--symbols", nargs="+", default=None, help="Symbols to download (default: top 40 futures)")
    parser.add_argument("--interval", default="4h", help="Bar interval")
    parser.add_argument("--days", type=int, default=2000, help="Days of history")
    parser.add_argument("--source", choices=["futures", "spot"], default="futures", help="Binance API")
    args = parser.parse_args()

    symbols = args.symbols or DEFAULT_SYMBOLS
    end_ms = int(time.time() * 1000)
    start_ms = end_ms - args.days * 24 * 3600 * 1000

    print(f"Downloading {len(symbols)} symbols, {args.interval} bars, {args.days} days from Binance {args.source}")

    success = 0
    failed = []
    for sym in symbols:
        filepath = DATA_DIR / f"{sym}_{args.interval}.csv"

        # Skip if already downloaded
        if filepath.exists():
            existing_lines = sum(1 for _ in open(filepath)) - 1
            if existing_lines > 100:
                print(f"  {sym}: already exists ({existing_lines} bars), skipping")
                success += 1
                continue

        print(f"  {sym}: downloading...", end="", flush=True)

        try:
            if args.source == "futures":
                bars = download_futures_klines(sym, args.interval, start_ms, end_ms)
            else:
                bars = download_spot_klines(sym, args.interval, start_ms, end_ms)

            if bars:
                save_csv(bars, str(filepath))
                print(f" {len(bars)} bars saved")
                success += 1
            else:
                print(f" no data!")
                failed.append(sym)
        except Exception as e:
            print(f" FAILED: {e}")
            failed.append(sym)

        time.sleep(0.3)

    print(f"\nDone: {success}/{len(symbols)} symbols downloaded")
    if failed:
        print(f"Failed: {', '.join(failed)}")

    # Print summary
    csv_files = sorted(DATA_DIR.glob("*.csv"))
    total_size = sum(f.stat().st_size for f in csv_files)
    print(f"\nData directory: {DATA_DIR}")
    print(f"Files: {len(csv_files)}, Total: {total_size / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main()
