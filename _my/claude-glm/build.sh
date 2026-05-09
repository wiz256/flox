#!/bin/bash
# Build and run the FLOX grid search
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building crush_grid ==="
cmake -B "$BUILD_DIR" \
    -DFLOX_ENABLE_BACKTEST=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -S "$SCRIPT_DIR"

cmake --build "$BUILD_DIR" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

echo ""
echo "=== Build complete ==="
echo ""
echo "Usage:"
echo "  $BUILD_DIR/crush_grid data/BTC_4H.csv          # uses all CPU cores"
echo "  $BUILD_DIR/crush_grid data/BTC_4H.csv 4        # uses 4 threads"
echo ""
echo "CSV format: timestamp,open,high,low,close,volume"
echo "  timestamp: Unix seconds, milliseconds, or ISO 8601"
echo ""
echo "To download sample data:"
echo "  python3 $SCRIPT_DIR/python/download_data.py"
