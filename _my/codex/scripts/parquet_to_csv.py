#!/usr/bin/env python3
"""Convert broker parquet bars into the CSV schema consumed by the C++ tool.

Required columns are timestamp/open/high/low/close/volume. The timestamp may be
named timestamp, timestamp_ms, open_time, open_time_ms, date, or datetime.

Example:
  python scripts/parquet_to_csv.py --in BTCUSDT_1m.parquet --out data/BTCUSDT_1m.csv
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd


TIME_CANDIDATES = ["timestamp_ms", "timestamp", "open_time_ms", "open_time", "date", "datetime"]


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--in", dest="input", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    df = pd.read_parquet(args.input)
    lower = {c.lower(): c for c in df.columns}
    time_col = next((lower[c] for c in TIME_CANDIDATES if c in lower), None)
    if time_col is None:
        raise SystemExit(f"No timestamp column found. Columns: {list(df.columns)}")

    out = pd.DataFrame()
    ts = df[time_col]
    if pd.api.types.is_datetime64_any_dtype(ts):
        out["timestamp_ms"] = (ts.astype("int64") // 1_000_000).astype("int64")
    else:
        raw = pd.to_numeric(ts)
        # Convert seconds/us/ns to ms by rough magnitude.
        if raw.max() < 1e12:
            raw = raw * 1000
        elif raw.max() > 1e15:
            raw = raw // 1_000_000
        elif raw.max() > 1e14:
            raw = raw // 1_000
        out["timestamp_ms"] = raw.astype("int64")

    for name in ["open", "high", "low", "close", "volume"]:
        if name not in lower:
            raise SystemExit(f"Missing required column {name!r}. Columns: {list(df.columns)}")
        out[name] = pd.to_numeric(df[lower[name]])

    out = out.sort_values("timestamp_ms").drop_duplicates("timestamp_ms")
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    out.to_csv(args.out, index=False)
    print(f"Wrote {len(out)} rows -> {args.out}")


if __name__ == "__main__":
    main()
