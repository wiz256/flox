#!/usr/bin/env python3
"""
Parity test: Python reference implementation of the C++ streaming engine.

Runs the exact same SMA Crossover + Chandelier exit logic as crush_grid.
Outputs trade-by-trade log so we can compare against C++ output.

Usage:
    python vendor/flox/_my/crush-glm/parity_test.py
"""
import csv
import math
from collections import deque
from dataclasses import dataclass
from pathlib import Path

DATA_DIR = Path(__file__).parent / "data"
FEE_RATE = 0.0004
INITIAL_CAPITAL = 10000.0


@dataclass
class Trade:
    entry_bar: int
    exit_bar: int
    side: int  # +1 long, -1 short
    entry_price: float
    exit_price: float
    pnl: float
    exit_reason: str


class StreamSma:
    def __init__(self, period):
        self.period = period
        self.buf = deque()

    def update(self, v):
        self.buf.append(v)
        if len(self.buf) > self.period:
            self.buf.popleft()

    def value(self):
        if len(self.buf) < self.period:
            return 0
        return sum(self.buf) / len(self.buf)

    def ready(self):
        return len(self.buf) >= self.period


class StreamAtr:
    def __init__(self, period):
        self.period = period
        self.n = 0
        self.val = 0.0
        self.prev_c = 0.0
        self.seeded = False

    def update(self, h, l, c):
        tr = max(h - l, abs(h - self.prev_c), abs(l - self.prev_c))
        if not self.seeded:
            self.val = tr
            self.seeded = True
        else:
            self.val = (self.val * (self.period - 1) + tr) / self.period
        self.prev_c = c
        self.n += 1

    def value(self):
        return self.val if self.n >= self.period else 0

    def ready(self):
        return self.n >= self.period


def run_parity(
    bars: list[dict],
    fast_period: int,
    slow_period: int,
    atr_period: int = 14,
    trail_mult: float = 3.0,
    sl_mult: float = 2.0,
    lookback: int = 12,
    all_equity: bool = True,
):
    """Run SMA crossover + chandelier exit, matching C++ engine logic exactly."""
    fast_sma = StreamSma(fast_period)
    slow_sma = StreamSma(slow_period)
    exit_atr = StreamAtr(atr_period)

    # State matching C++ BaseStrategy
    side = 0
    pos_live = False
    entry_price = 0.0
    high_since = 0.0
    low_since = 1e18
    bars_in_trade = 0
    be_act = False
    trail = 0.0
    prev_atr = 0.0
    atr_at_entry = 0.0
    hard_sl_long = 0.0
    hard_sl_short = 1e18

    pending_entry = 0
    pending_signal_exit = False
    pending_risk_exit = False

    high_window = deque()
    low_window = deque()

    prev_above = False
    trades: list[Trade] = []
    equity = INITIAL_CAPITAL

    for bar_idx, bar in enumerate(bars):
        o = bar["open"]
        h = bar["high"]
        l = bar["low"]
        c = bar["close"]

        # --- STEP 1: Execute PENDING actions (1-bar shift, fills at OPEN) ---
        if pending_risk_exit and pos_live:
            qty = equity / entry_price if all_equity else 1000.0 / entry_price
            pnl = qty * (o - entry_price) * side
            fee = abs(qty * o) * FEE_RATE
            trades.append(Trade(
                entry_bar=0, exit_bar=bar_idx, side=side,
                entry_price=entry_price, exit_price=o,
                pnl=pnl - fee, exit_reason="risk",
            ))
            equity += pnl - fee
            side = 0
            pos_live = False
            bars_in_trade = 0
            be_act = False
            trail = 0.0
            high_window.clear()
            low_window.clear()
        elif pending_signal_exit and pos_live:
            qty = equity / entry_price if all_equity else 1000.0 / entry_price
            pnl = qty * (o - entry_price) * side
            fee = abs(qty * o) * FEE_RATE
            trades[-1] = Trade(
                entry_bar=trades[-1].entry_bar if trades else 0,
                exit_bar=bar_idx, side=side,
                entry_price=entry_price, exit_price=o,
                pnl=pnl - fee, exit_reason="signal",
            )
            equity += pnl - fee
            side = 0
            pos_live = False
            bars_in_trade = 0
            be_act = False
            trail = 0.0
            high_window.clear()
            low_window.clear()

        if pending_entry != 0 and not pos_live:
            entry_price = o
            high_since = o
            low_since = o
            bars_in_trade = 0
            side = pending_entry
            pos_live = True
            be_act = False
            trail = 0.0
            atr_at_entry = prev_atr
            hard_sl_long = entry_price - sl_mult * atr_at_entry if atr_at_entry > 0 else 0
            hard_sl_short = entry_price + sl_mult * atr_at_entry if atr_at_entry > 0 else 1e18
            high_window.clear()
            low_window.clear()

        pending_entry = 0
        pending_signal_exit = False
        pending_risk_exit = False

        # --- STEP 2: Update indicators ---
        use_atr = exit_atr.value() if exit_atr.ready() else 0
        fast_sma.update(c)
        slow_sma.update(c)
        exit_atr.update(h, l, c)
        prev_atr = use_atr

        # --- STEP 3: Check risk exit (chandelier) ---
        if side != 0 and exit_atr.ready():
            atr = use_atr
            if atr > 0:
                bars_in_trade += 1

                if side == 1:
                    # Anchor BEFORE current bar
                    if lookback > 0 and len(high_window) >= lookback:
                        anchor = max(high_window)
                    else:
                        anchor = high_since
                    high_since = max(high_since, h)
                    high_window.append(h)
                    if lookback > 0 and len(high_window) > lookback:
                        high_window.popleft()

                    trail_level = anchor - trail_mult * atr
                    level = max(trail_level, hard_sl_long)
                    if l <= level:
                        pending_risk_exit = True
                        side = 0
                    elif lookback > 0 and bars_in_trade >= 40:
                        pending_risk_exit = True
                        side = 0

                elif side == -1:
                    if lookback > 0 and len(low_window) >= lookback:
                        anchor = min(low_window)
                    else:
                        anchor = low_since
                    low_since = min(low_since, l)
                    low_window.append(l)
                    if lookback > 0 and len(low_window) > lookback:
                        low_window.popleft()

                    trail_level = anchor + trail_mult * atr
                    level = min(trail_level, hard_sl_short)
                    if h >= level:
                        pending_risk_exit = True
                        side = 0
                    elif lookback > 0 and bars_in_trade >= 40:
                        pending_risk_exit = True
                        side = 0

        # --- STEP 4: Generate signal (SMA crossover, no signal exits in chan mode) ---
        if fast_sma.ready() and slow_sma.ready():
            above = fast_sma.value() > slow_sma.value()
            if side == 0 and above and not prev_above:
                pending_entry = 1
                pending_signal_exit = False
                pending_risk_exit = False
                side = 1
            elif side == 0 and not above and prev_above:
                pending_entry = -1
                pending_signal_exit = False
                pending_risk_exit = False
                side = -1
            prev_above = above

    return trades, equity


def load_bars(csv_path: Path, years_back: int = 0) -> list[dict]:
    bars = []
    with open(csv_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            bars.append({
                "timestamp": int(row["timestamp"]),
                "open": float(row["open"]),
                "high": float(row["high"]),
                "low": float(row["low"]),
                "close": float(row["close"]),
                "volume": float(row["volume"]),
            })
    if years_back > 0:
        ms_per_year = 365.25 * 24 * 3600 * 1000
        cutoff = bars[-1]["timestamp"] - int(years_back * ms_per_year)
        bars = [b for b in bars if b["timestamp"] >= cutoff]
    return bars


def sharpe_from_trades(trades, n_bars, ann_factor=2190):
    if not trades:
        return 0.0
    # Build per-bar returns from equity curve
    equity = INITIAL_CAPITAL
    eq_curve = [equity] * (n_bars + 1)
    trade_idx = 0
    for i in range(n_bars):
        if trade_idx < len(trades) and trades[trade_idx].exit_bar == i:
            equity += trades[trade_idx].pnl
            trade_idx += 1
        eq_curve[i + 1] = equity

    returns = []
    for i in range(1, len(eq_curve)):
        if eq_curve[i - 1] > 0:
            returns.append(eq_curve[i] / eq_curve[i - 1] - 1)

    if len(returns) < 2:
        return 0.0
    mean = sum(returns) / len(returns)
    var = sum(r * r for r in returns) / len(returns) - mean * mean
    if var <= 0:
        return 0.0
    return (mean / math.sqrt(var)) * math.sqrt(ann_factor)


if __name__ == "__main__":
    csv_path = DATA_DIR / "BTCUSDTUSDT_4h.csv"
    if not csv_path.exists():
        # Try symlink
        csv_path = DATA_DIR / "BTCUSDT_4h.csv"

    bars = load_bars(csv_path, years_back=1)
    print(f"Loaded {len(bars)} bars")

    configs = [
        ("SMA(10,40) Chan trail=2.0 sl=1.2 lb=12", 10, 40, 14, 2.0, 1.2, 12),
        ("SMA(10,40) Chan trail=3.0 sl=2.0 lb=8", 10, 40, 14, 3.0, 2.0, 8),
        ("SMA(20,50) Chan trail=2.0 sl=1.2 lb=12", 20, 50, 14, 2.0, 1.2, 12),
        ("SMA(10,40) Chan trail=4.0 sl=2.0 lb=0", 10, 40, 14, 4.0, 2.0, 0),
    ]

    print(f"\n{'Config':<50} {'Trades':>6} {'Sharpe':>8} {'Return':>10} {'Equity':>10}")
    print("-" * 90)

    for label, fast, slow, atr_per, trail, sl, lb in configs:
        trades, final_eq = run_parity(
            bars, fast, slow, atr_per, trail, sl, lb, all_equity=True
        )
        sh = sharpe_from_trades(trades, len(bars))
        ret = final_eq - INITIAL_CAPITAL
        print(f"{label:<50} {len(trades):>6} {sh:>8.3f} {ret:>10.2f} {final_eq:>10.2f}")

        if len(trades) <= 20:
            print(f"\n  Trade log for: {label}")
            for i, t in enumerate(trades):
                direction = "LONG " if t.side == 1 else "SHORT"
                print(f"    #{i+1:2d} {direction} entry={t.entry_price:>10.2f} "
                      f"exit={t.exit_price:>10.2f} pnl={t.pnl:>8.2f} "
                      f"bar={t.exit_bar:>5d} reason={t.exit_reason}")
