#!/usr/bin/env python3
"""Record live trades/books into FLOX .floxlog segments via FLOX's CCXT adapter.

This is a starter ingestor, not a strategy. Keep it separate from trading:
  market data -> recorder -> daily/hourly immutable segments -> validation ->
  mmap bar preaggregation.

It requires flox_py and ccxt.pro support in your environment.
"""

from __future__ import annotations

import argparse
import asyncio
from pathlib import Path


async def main_async() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--exchange", default="binance")
    p.add_argument("--symbols", required=True, help="Comma-separated CCXT symbols, e.g. BTC/USDT,ETH/USDT")
    p.add_argument("--out", required=True)
    p.add_argument("--seconds", type=int, default=3600)
    p.add_argument("--book-depth", type=int, default=20)
    args = p.parse_args()

    import flox_py as flox
    from flox_py.ccxt import CcxtBroker

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    async with CcxtBroker(args.exchange) as broker:
        for sym in [s.strip() for s in args.symbols.split(",") if s.strip()]:
            await broker.add_symbol(sym)

        # The FLOX docs expose MarketDataRecorder through the Runner/CCXT path.
        # Exact binding names can move, so fail loudly if the installed wheel is
        # older than the docs in this checkout.
        if not hasattr(flox, "MarketDataRecorder"):
            raise RuntimeError("Installed flox_py does not expose MarketDataRecorder")

        recorder = flox.MarketDataRecorder(str(out))
        broker._runner.set_market_data_recorder(recorder)  # documented internal wiring point

        task = asyncio.create_task(
            broker.run(streams=("trades", "book"), book_depth=args.book_depth)
        )
        try:
            await asyncio.sleep(args.seconds)
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)


def main() -> None:
    asyncio.run(main_async())


if __name__ == "__main__":
    main()
