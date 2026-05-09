#!/usr/bin/env python3
"""Download Binance USD-M futures klines as appendable CSV partitions.

This is deliberately boring and durable:
  - It uses only the Python standard library.
  - It appends from the last timestamp already present in the CSV.
  - It writes broker candles first; the C++ tool converts those candles into
    native FLOX mmap bars for fast C++ grid backtests.

Example:
  python scripts/binance_futures_klines.py \
    --symbols BTCUSDT,ETHUSDT --interval 1m --start 2024-01-01 --out data/binance_csv
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import time
import urllib.parse
import urllib.request
from pathlib import Path

BASE = "https://fapi.binance.com/fapi/v1/klines"


def parse_utc(text: str) -> int:
    if text.isdigit():
        return int(text)
    value = dt.datetime.fromisoformat(text.replace("Z", "+00:00"))
    if value.tzinfo is None:
        value = value.replace(tzinfo=dt.timezone.utc)
    return int(value.timestamp() * 1000)


def last_open_ms(path: Path) -> int | None:
    if not path.exists() or path.stat().st_size == 0:
        return None
    last = None
    with path.open("r", newline="") as f:
        for row in csv.DictReader(f):
            last = row
    return int(last["timestamp_ms"]) if last else None


def fetch(symbol: str, interval: str, start_ms: int, end_ms: int | None) -> list[list]:
    params = {
        "symbol": symbol,
        "interval": interval,
        "startTime": start_ms,
        "limit": 1500,
    }
    if end_ms is not None:
        params["endTime"] = end_ms
    url = BASE + "?" + urllib.parse.urlencode(params)
    with urllib.request.urlopen(url, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def update_symbol(symbol: str, interval: str, start_ms: int, end_ms: int | None, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{symbol}_{interval}.csv"
    existing_last = last_open_ms(path)
    cursor = max(start_ms, existing_last + 1 if existing_last is not None else start_ms)

    write_header = not path.exists() or path.stat().st_size == 0
    rows_written = 0
    with path.open("a", newline="") as f:
      writer = csv.writer(f)
      if write_header:
          writer.writerow(["timestamp_ms", "open", "high", "low", "close", "volume"])
      while True:
          batch = fetch(symbol, interval, cursor, end_ms)
          if not batch:
              break
          for k in batch:
              open_ms = int(k[0])
              if existing_last is not None and open_ms <= existing_last:
                  continue
              writer.writerow([open_ms, k[1], k[2], k[3], k[4], k[5]])
              rows_written += 1
              cursor = open_ms + 1
          f.flush()
          if len(batch) < 1500:
              break
          time.sleep(0.08)
    print(f"{symbol}: wrote {rows_written} rows -> {path}")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--symbols", required=True, help="Comma-separated, e.g. BTCUSDT,ETHUSDT")
    p.add_argument("--interval", default="1m")
    p.add_argument("--start", required=True, help="UTC date or millisecond timestamp")
    p.add_argument("--end", default=None, help="UTC date or millisecond timestamp")
    p.add_argument("--out", required=True)
    args = p.parse_args()

    start_ms = parse_utc(args.start)
    end_ms = parse_utc(args.end) if args.end else None
    for symbol in [s.strip().upper() for s in args.symbols.split(",") if s.strip()]:
        update_symbol(symbol, args.interval, start_ms, end_ms, Path(args.out))


if __name__ == "__main__":
    main()
