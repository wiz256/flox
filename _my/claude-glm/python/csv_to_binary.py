"""
Convert OHLCV CSV files to FLOX binary .floxlog format for faster loading.

The binary format (per bar):
  [8 bytes] timestamp_ns (int64, Unix nanoseconds)
  [8 bytes] open         (int64, fixed-point raw, 8 decimals)
  [8 bytes] high         (int64, fixed-point raw, 8 decimals)
  [8 bytes] low          (int64, fixed-point raw, 8 decimals)
  [8 bytes] close        (int64, fixed-point raw, 8 decimals)
  [8 bytes] volume       (int64, fixed-point raw, 8 decimals)
  = 48 bytes per bar

A 4H bar file with 6 years of data (~13,000 bars) = ~600 KB.

Usage:
    python3 csv_to_binary.py data/BTC_4H.csv data/BTC_4H.bin
    python3 csv_to_binary.py data/  # converts all CSV in directory
"""

import argparse
import csv
import struct
import sys
from pathlib import Path

# FLOX fixed-point scale: 8 decimal places
SCALE = 100_000_000


def price_to_raw(price: float) -> int:
    """Convert float price to FLOX fixed-point raw int."""
    return int(round(price * SCALE))


def parse_timestamp(val: str) -> int:
    """Parse timestamp to Unix nanoseconds."""
    try:
        ts = float(val)
        if ts < 1e11:
            return int(ts * 1e9)
        elif ts < 1e14:
            return int(ts * 1e6)
        else:
            return int(ts)
    except ValueError:
        pass

    # ISO 8601 parsing
    from datetime import datetime
    for fmt in ["%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S",
                "%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%dT%H:%M:%S.%f"]:
        try:
            dt = datetime.strptime(val, fmt)
            return int(dt.timestamp() * 1e9)
        except ValueError:
            continue
    raise ValueError(f"Cannot parse timestamp: {val}")


def convert_csv_to_bin(csv_path: str, bin_path: str) -> int:
    """Convert one CSV file to binary. Returns number of bars written."""
    count = 0
    with open(csv_path) as fin, open(bin_path, "wb") as fout:
        reader = csv.DictReader(fin)
        for row in reader:
            ts_ns = parse_timestamp(row.get("timestamp", "0"))
            o = price_to_raw(float(row["open"]))
            h = price_to_raw(float(row["high"]))
            l = price_to_raw(float(row["low"]))
            c = price_to_raw(float(row["close"]))
            v = price_to_raw(float(row.get("volume", "0")))

            fout.write(struct.pack("<qqqqqq", ts_ns, o, h, l, c, v))
            count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description="Convert OHLCV CSV to FLOX binary")
    parser.add_argument("input", help="CSV file or directory")
    parser.add_argument("output", nargs="?", help="Output file (only for single file)")
    args = parser.parse_args()

    input_path = Path(args.input)

    if input_path.is_dir():
        # Convert all CSV files in directory
        csv_files = list(input_path.glob("*.csv"))
        if not csv_files:
            print(f"No CSV files found in {input_path}")
            sys.exit(1)

        for csv_file in csv_files:
            bin_file = csv_file.with_suffix(".bin")
            count = convert_csv_to_bin(str(csv_file), str(bin_file))
            csv_size = csv_file.stat().st_size
            bin_size = bin_file.stat().st_size
            ratio = csv_size / bin_size if bin_size > 0 else 0
            print(f"  {csv_file.name}: {count} bars, {csv_size/1024:.0f}KB -> {bin_size/1024:.0f}KB ({ratio:.1f}x smaller)")
    else:
        output = args.output or str(input_path.with_suffix(".bin"))
        count = convert_csv_to_bin(str(input_path), output)
        print(f"Converted {count} bars: {input_path} -> {output}")


if __name__ == "__main__":
    main()
