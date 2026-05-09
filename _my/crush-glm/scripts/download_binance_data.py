#!/usr/bin/env python3
"""Download Binance USDM Futures klines and convert to FLOX-compatible CSV.

Usage:
    python vendor/flox/_my/scripts/download_binance_data.py
    python vendor/flox/_my/scripts/download_binance_data.py --symbols BTCUSDT ETHUSDT --timeframes 4h 1h
    python vendor/flox/_my/scripts/download_binance_data.py --convert-only

Output: vendor/flox/_my/data/{SYMBOL}_{TF}.csv
Format: timestamp,open,high,low,close,volume  (FLOX CsvOhlcvReader compatible)
"""
import argparse
import os
import sys
import time
from pathlib import Path

try:
    import pandas as pd
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False

try:
    import urllib.request
    import json
    HAS_URLLIB = True
except ImportError:
    HAS_URLLIB = False

BASE_URL = "https://fapi.binance.com/fapi/v1/klines"
DATA_DIR = Path(__file__).resolve().parent.parent / "data"
EXISTING_DATA_DIR = Path("/Users/lex/WorkspaceTrading/_data")

DEFAULT_SYMBOLS = [
    "BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT",
    "ADAUSDT", "DOGEUSDT", "AVAXUSDT", "DOTUSDT", "LINKUSDT",
]

DEFAULT_TIMEFRAMES = ["1h", "4h", "1d"]

INTERVAL_MS = {
    "1m": 60_000, "3m": 180_000, "5m": 300_000, "15m": 900_000,
    "30m": 1_800_000, "1h": 3_600_000, "2h": 7_200_000,
    "4h": 14_400_000, "6h": 21_600_000, "8h": 28_800_000,
    "12h": 43_200_000, "1d": 86_400_000, "3d": 259_200_000,
    "1w": 604_800_000, "1M": 2_592_000_000,
}


def fetch_klines(symbol: str, interval: str, start_ms: int, end_ms: int) -> list:
    all_rows = []
    current = start_ms
    while current < end_ms:
        params = f"?symbol={symbol}&interval={interval}&startTime={current}&endTime={end_ms}&limit=1500"
        url = BASE_URL + params
        retries = 3
        for attempt in range(retries):
            try:
                req = urllib.request.Request(url)
                with urllib.request.urlopen(req, timeout=30) as resp:
                    data = json.loads(resp.read().decode())
                break
            except Exception as e:
                if attempt == retries - 1:
                    print(f"  ERROR fetching {symbol} {interval}: {e}")
                    return all_rows
                time.sleep(2 ** attempt)
        if not data:
            break
        all_rows.extend(data)
        current = data[-1][0] + INTERVAL_MS.get(interval, 3_600_000)
        time.sleep(0.1)
    return all_rows


def klines_to_csv(rows: list, csv_path: Path):
    with open(csv_path, "w") as f:
        f.write("timestamp,open,high,low,close,volume\n")
        for row in rows:
            ts = row[0]
            o, h, l, c, v = row[1], row[2], row[3], row[4], row[5]
            f.write(f"{ts},{o},{h},{l},{c},{v}\n")


def convert_existing_parquet():
    if not HAS_PANDAS:
        print("pandas required for --convert-only")
        return
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    converted = 0
    for subdir in ["binanceusdm", "candles_crypto"]:
        base = EXISTING_DATA_DIR / subdir
        if not base.exists():
            continue
        for pq in base.glob("**/*.parquet"):
            symbol_tf = pq.stem.split("_")[0] if "_" in pq.stem else pq.stem
            parent_name = pq.parent.name
            if subdir == "binanceusdm":
                symbol = parent_name
                tf = pq.stem
            else:
                parts = pq.stem.split("_")
                if len(parts) >= 2:
                    symbol = parts[0]
                    tf = parts[1]
                else:
                    continue
            tf_clean = tf.replace("-", "").lower()
            if tf_clean not in INTERVAL_MS:
                continue
            out = DATA_DIR / f"{symbol}_{tf_clean}.csv"
            if out.exists():
                continue
            try:
                df = pd.read_parquet(pq)
                if isinstance(df.columns, pd.MultiIndex):
                    df.columns = df.columns.get_level_values(-1)
                col_map = {}
                for col in df.columns:
                    cl = col.lower()
                    if cl in ("open", "high", "low", "close", "volume"):
                        col_map[cl] = col
                    elif cl in ("timestamp", "date", "datetime", "time", "index"):
                        col_map["timestamp"] = col
                if not all(k in col_map for k in ("open", "high", "low", "close", "volume")):
                    continue
                ts_col = col_map.get("timestamp")
                if ts_col and ts_col in df.columns:
                    ts = pd.to_datetime(df[ts_col]).astype("int64") // 1_000_000
                elif isinstance(df.index, pd.DatetimeIndex):
                    ts = df.index.astype("int64") // 1_000_000
                else:
                    continue
                out_df = pd.DataFrame({
                    "timestamp": ts.values,
                    "open": df[col_map["open"]].values.astype(float),
                    "high": df[col_map["high"]].values.astype(float),
                    "low": df[col_map["low"]].values.astype(float),
                    "close": df[col_map["close"]].values.astype(float),
                    "volume": df[col_map["volume"]].values.astype(float),
                })
                out_df.to_csv(out, index=False)
                converted += 1
                print(f"  Converted: {pq.name} -> {out.name}")
            except Exception as e:
                print(f"  Skip {pq.name}: {e}")
    print(f"Converted {converted} files")


def download_fresh(symbols: list, timeframes: list, start_date: str):
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    import datetime
    start_ms = int(datetime.datetime.strptime(start_date, "%Y-%m-%d").timestamp() * 1000)
    end_ms = int(time.time() * 1000)
    total = len(symbols) * len(timeframes)
    done = 0
    for symbol in symbols:
        for tf in timeframes:
            done += 1
            csv_path = DATA_DIR / f"{symbol}_{tf}.csv"
            if csv_path.exists():
                print(f"[{done}/{total}] {symbol} {tf}: already exists, skipping")
                continue
            print(f"[{done}/{total}] {symbol} {tf}: downloading...", end=" ", flush=True)
            rows = fetch_klines(symbol, tf, start_ms, end_ms)
            if rows:
                klines_to_csv(rows, csv_path)
                print(f"{len(rows)} bars")
            else:
                print("no data")
            time.sleep(0.5)
    print(f"\nData saved to {DATA_DIR}/")


def main():
    parser = argparse.ArgumentParser(description="Download/convert Binance Futures data for FLOX")
    parser.add_argument("--symbols", nargs="+", default=DEFAULT_SYMBOLS)
    parser.add_argument("--timeframes", nargs="+", default=DEFAULT_TIMEFRAMES)
    parser.add_argument("--start", default="2020-01-01", help="Start date (YYYY-MM-DD)")
    parser.add_argument("--convert-only", action="store_true", help="Only convert existing parquet data")
    args = parser.parse_args()

    print(f"Output directory: {DATA_DIR}")
    if args.convert_only:
        convert_existing_parquet()
    else:
        convert_existing_parquet()
        download_fresh(args.symbols, args.timeframes, args.start)


if __name__ == "__main__":
    main()
