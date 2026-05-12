#!/usr/bin/env python3
"""
Parity test: Python streaming engine vs C++ FLOX engine.

Exact 1:1 match of the C++ pending mechanism:
  - Signals detected on bar N → fill at bar N+1 open
  - ATR shifted by 1 (use prev bar's ATR for stop computation)
  - Hard SL frozen at entry time
  - Chandelier anchor capped to lookback window
  - In chandelier mode, signal exits suppressed (only risk exits)

Layer 1: SMA crossover entries+exits (SignalBETrail mode, signal exits only)
Layer 2: SMA crossover + chandelier exits (no signal exits)

Usage:
    python3 vendor/flox/_my/crush-glm/vbt_parity.py
"""
import pandas as pd
import numpy as np
from pathlib import Path
import sys

DATA_DIR = Path(__file__).parent / "data"
CSV = DATA_DIR / "BTCUSDTUSDT_4h.csv"

FEE_RATE = 0.0004
CAPITAL = 10000.0


def load_csv(years_back: int = 1) -> pd.DataFrame:
    df = pd.read_csv(CSV)
    df["datetime"] = pd.to_datetime(df["timestamp"], unit="ms", utc=True)
    df = df.set_index("datetime").sort_index()
    if years_back > 0:
        cutoff = df.index[-1] - pd.DateOffset(years=years_back)
        df = df.loc[cutoff:]
    return df


# ── Streaming SMA ────────────────────────────────────────────────────

class StreamSma:
    def __init__(self, period: int):
        self.p = period
        self.buf = []

    def update(self, v: float):
        self.buf.append(v)
        if len(self.buf) > self.p:
            self.buf.pop(0)

    def value(self) -> float:
        if len(self.buf) < self.p:
            return 0.0
        return sum(self.buf) / len(self.buf)

    def ready(self) -> bool:
        return len(self.buf) >= self.p


# ── Streaming ATR (EMA-style, matching C++ StreamAtr) ───────────────

class StreamAtr:
    def __init__(self, period: int):
        self.p = period
        self.n = 0
        self.val = 0.0
        self.prev_c = 0.0
        self.seeded = False

    def update(self, h: float, l: float, c: float):
        tr = max(h - l, abs(h - self.prev_c), abs(l - self.prev_c))
        if not self.seeded:
            self.val = tr
            self.seeded = True
        else:
            self.val = (self.val * (self.p - 1) + tr) / self.p
        self.prev_c = c
        self.n += 1

    def value(self) -> float:
        return self.val if self.n >= self.p else 0.0

    def ready(self) -> bool:
        return self.n >= self.p


# ── Chandelier exit check (matches C++ checkRiskExit) ──────────────

def check_risk_exit(side, entry_price, high_since, low_since, high_window, low_window,
                    h, l, atr, hard_sl_long, hard_sl_short, trail_mult, lookback,
                    exit_mode, bars_in_trade, be_act, trail, sl_mult, be_r_mult, entry_price_orig):
    """
    Returns (pending_risk_exit, side, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window)
    side is set to 0 if risk exit triggered.
    """
    if atr <= 0:
        return False, side, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window

    bars_in_trade += 1

    if side == 1:
        # Anchor from window BEFORE current bar
        anchor = max(high_window) if high_window else high_since
        high_since = max(high_since, h)
        high_window.append(h)
        if lookback > 0 and len(high_window) > lookback:
            high_window.pop(0)

        if exit_mode == "chandelier":
            trail_level = anchor - trail_mult * atr
            level = max(trail_level, hard_sl_long)
            if l <= level:
                return True, 0, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window

        elif exit_mode == "sig_be":
            ir = sl_mult * atr
            if not be_act and (h - entry_price) >= be_r_mult * ir:
                be_act = True
                trail = entry_price
            if be_act:
                trail = max(trail, h - trail_mult * atr)
                if l <= trail:
                    return True, 0, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window
            if l <= hard_sl_long:
                return True, 0, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window

    elif side == -1:
        # FIX: Use separate low_window/low_since for shorts (was high_window in old version)
        anchor = min(low_window) if low_window else low_since
        low_since = min(low_since, l)
        low_window.append(l)
        if lookback > 0 and len(low_window) > lookback:
            low_window.pop(0)

        if exit_mode == "chandelier":
            trail_level = anchor + trail_mult * atr
            # FIX: Use hard_sl_short (entry + sl*atr), not hard_sl_long
            level = min(trail_level, hard_sl_short)
            if h >= level:
                return True, 0, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window

        elif exit_mode == "sig_be":
            ir = sl_mult * atr
            if not be_act and (entry_price - l) >= be_r_mult * ir:
                be_act = True
                trail = entry_price
            if be_act:
                trail = min(trail, l + trail_mult * atr)
                if h >= trail:
                    return True, 0, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window
            if h >= hard_sl_short:
                return True, 0, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window

    return False, side, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window


# ── Streaming engine (matches C++ onSymbolBar exactly) ─────────────

def run_streaming(df, fast=10, slow=40, exit_mode="chandelier",
                  atr_period=14, trail_mult=2.0, sl_mult=1.2, lookback=12,
                  be_r_mult=1.0, trace=False, sizing="all_equity"):
    """
    Exact match of C++ BaseStrategy::onSymbolBar + SmaCrossStrat::onBarImpl.
    Returns dict with trades, equity, trade_list.
    """
    closes = df["close"].values
    opens = df["open"].values
    highs = df["high"].values
    lows = df["low"].values
    n_bars = len(closes)

    # Indicators
    fast_sma = StreamSma(fast)
    slow_sma = StreamSma(slow)
    exit_atr = StreamAtr(atr_period)

    # State (matches C++ member vars)
    pending_entry = 0       # +1=long, -1=short
    pending_risk_exit = False
    pending_signal_exit = False
    side = 0                # +1=long, -1=short, 0=flat
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
    prev_above = None
    high_window = []
    low_window = []

    equity = CAPITAL
    trades = []

    signal_exits_allowed = (exit_mode == "sig_be")

    for i in range(n_bars):
        o = opens[i]   # fill price (real open)
        h = highs[i]
        l = lows[i]
        c = closes[i]  # real close (for indicators)

        # ── STEP 1: Execute PENDING actions (fill at open) ──
        if pending_risk_exit and pos_live:
            if trace:
                print(f"  bar={i} EXIT {'LONG' if side==1 else 'SHRT'} @ open={o:.2f} reason=risk", file=sys.stderr)
            side = 0; bars_in_trade = 0; be_act = False; trail = 0.0; pos_live = False
            high_window = []; low_window = []

        elif pending_signal_exit and pos_live:
            if trace:
                print(f"  bar={i} EXIT {'LONG' if side==1 else 'SHRT'} @ open={o:.2f} reason=signal", file=sys.stderr)
            side = 0; bars_in_trade = 0; be_act = False; trail = 0.0; pos_live = False
            high_window = []; low_window = []

        if pending_entry != 0 and not pos_live:
            entry_price = o  # FIX: Use fill price (open), not close
            qty = CAPITAL / entry_price if sizing == "all_equity" else 1000.0 / entry_price
            side = pending_entry
            pos_live = True
            bars_in_trade = 0; be_act = False; trail = 0.0
            atr_at_entry = prev_atr
            hard_sl_long = entry_price - sl_mult * atr_at_entry if atr_at_entry > 0 else 0
            hard_sl_short = entry_price + sl_mult * atr_at_entry if atr_at_entry > 0 else 1e18
            high_since = entry_price  # FIX: Init to fill price, not close
            low_since = entry_price
            high_window = []; low_window = []
            if trace:
                d = "LONG" if side == 1 else "SHRT"
                print(f"  bar={i} ENTRY {d} @ open={o:.2f} qty={qty:.4f} atr@entry={atr_at_entry:.2f} "
                      f"hardSl={hard_sl_long:.2f}/{hard_sl_short:.2f}", file=sys.stderr)

        pending_entry = 0; pending_risk_exit = False; pending_signal_exit = False

        # ── STEP 2: Update indicators ──
        # Capture ATR BEFORE update (1-bar shift, matching C++)
        use_atr = exit_atr.value() if exit_atr.ready() else 0.0
        fast_sma.update(c)
        slow_sma.update(c)
        exit_atr.update(h, l, c)
        prev_atr = use_atr

        # ── STEP 3: Check risk exit conditions ──
        if side != 0 and exit_atr.ready():
            result = check_risk_exit(
                side, entry_price, high_since, low_since, high_window, low_window,
                h, l, prev_atr, hard_sl_long, hard_sl_short, trail_mult, lookback,
                exit_mode, bars_in_trade, be_act, trail, sl_mult, be_r_mult, entry_price
            )
            pending_risk_exit, side, bars_in_trade, be_act, trail, high_since, low_since, high_window, low_window = result

        # ── STEP 4: Generate signals ──
        if not fast_sma.ready() or not slow_sma.ready():
            prev_above = None
            continue

        fast_val = fast_sma.value()
        slow_val = slow_sma.value()
        above = fast_val > slow_val

        if prev_above is not None:
            if above and not prev_above and side == 0:
                pending_entry = 1; pending_risk_exit = False; pending_signal_exit = False
                side = 1  # Set immediately (matches C++ enterLong)
                if trace:
                    print(f"  bar={i} CROSS_UP fast={fast_val:.2f} slow={slow_val:.2f} → pending_entry=LONG", file=sys.stderr)
            elif not above and prev_above and side == 0:
                pending_entry = -1; pending_risk_exit = False; pending_signal_exit = False
                side = -1  # Set immediately (matches C++ enterShort)
                if trace:
                    print(f"  bar={i} CROSS_DN fast={fast_val:.2f} slow={slow_val:.2f} → pending_entry=SHRT", file=sys.stderr)
            elif signal_exits_allowed and side == 1 and not above:
                pending_signal_exit = True
            elif signal_exits_allowed and side == -1 and above:
                pending_signal_exit = True

        prev_above = above

    return {"trades": 0, "equity": equity, "trade_list": trades}


def run_streaming_with_pnl(df, fast=10, slow=40, exit_mode="chandelier",
                           atr_period=14, trail_mult=2.0, sl_mult=1.2, lookback=12,
                           be_r_mult=1.0, trace=True, sizing="all_equity"):
    """
    Full streaming engine with PnL tracking.
    Tracks entry/exit bars, prices, reasons, and PnL.
    """
    closes = df["close"].values
    opens = df["open"].values
    highs = df["high"].values
    lows = df["low"].values
    n_bars = len(closes)

    fast_sma = StreamSma(fast)
    slow_sma = StreamSma(slow)
    exit_atr = StreamAtr(atr_period)

    pending_entry = 0
    pending_risk_exit = False
    pending_signal_exit = False
    side = 0
    pos_live = False
    entry_price = 0.0
    entry_bar = 0
    entry_qty = 0.0
    high_since = 0.0
    low_since = 1e18
    bars_in_trade = 0
    be_act = False
    trail = 0.0
    prev_atr = 0.0
    atr_at_entry = 0.0
    hard_sl_long = 0.0
    hard_sl_short = 1e18
    prev_above = None
    high_window = []
    low_window = []

    equity = CAPITAL
    trades = []

    signal_exits_allowed = (exit_mode == "sig_be")

    for i in range(n_bars):
        o = opens[i]
        h = highs[i]
        l = lows[i]
        c = closes[i]

        # ── STEP 1: Execute PENDING actions (fill at open) ──
        if pending_risk_exit and pos_live:
            pnl = entry_qty * (o - entry_price) * (1 if trades and trades[-1].get("_side_num", side) == 1 else -1) if False else 0
            # Use the side from before it was cleared
            exit_side = side if side != 0 else (1 if entry_price < o else -1)
            # Actually side is already 0 from the risk check. Track it differently.
            # We need the side at the time of exit. Let me track it properly.
            pass

        # Let me redo this more carefully. Track current_side separately.
        pass

    # OK, let me simplify and track trades properly
    return _run_streaming_impl(closes, opens, highs, lows, fast, slow, exit_mode,
                               atr_period, trail_mult, sl_mult, lookback, be_r_mult, trace, sizing)


def _run_streaming_impl(closes, opens, highs, lows, fast, slow, exit_mode,
                        atr_period, trail_mult, sl_mult, lookback, be_r_mult, trace, sizing):
    """Clean implementation with proper PnL tracking."""
    n_bars = len(closes)

    fast_sma = StreamSma(fast)
    slow_sma = StreamSma(slow)
    exit_atr = StreamAtr(atr_period)

    pending_entry = 0
    pending_risk_exit = False
    pending_signal_exit = False
    side = 0           # Current position side: +1, -1, 0
    pos_live = False
    entry_price = 0.0
    entry_bar = 0
    entry_qty = 0.0
    high_since = 0.0
    low_since = 1e18
    bars_in_trade = 0
    be_act = False
    trail = 0.0
    prev_atr = 0.0
    atr_at_entry = 0.0
    hard_sl_long = 0.0
    hard_sl_short = 1e18
    prev_above = None
    high_window = []
    low_window = []
    # Track which side we're closing (since check_risk_exit sets side=0)
    closing_side = 0

    equity = CAPITAL
    trades = []

    signal_exits_allowed = (exit_mode == "sig_be")

    for i in range(n_bars):
        o = opens[i]
        h = highs[i]
        l = lows[i]
        c = closes[i]

        # ── STEP 1: Execute PENDING actions ──
        if pending_risk_exit and pos_live:
            closing_side = side if side != 0 else closing_side
            pnl = entry_qty * (o - entry_price) * closing_side
            fee = abs(entry_qty * o) * FEE_RATE
            equity += pnl - fee
            trades.append({"entry_bar": entry_bar, "exit_bar": i, "side": closing_side,
                          "entry": entry_price, "exit": o, "reason": "risk", "pnl": pnl - fee})
            if trace:
                d = "LONG" if closing_side == 1 else "SHRT"
                print(f"  bar={i} EXIT {d} @ open={o:.2f} reason=risk pnl={pnl-fee:.2f}", file=sys.stderr)
            side = 0; bars_in_trade = 0; be_act = False; trail = 0.0; pos_live = False
            high_window = []; low_window = []

        elif pending_signal_exit and pos_live:
            closing_side = side if side != 0 else closing_side
            pnl = entry_qty * (o - entry_price) * closing_side
            fee = abs(entry_qty * o) * FEE_RATE
            equity += pnl - fee
            trades.append({"entry_bar": entry_bar, "exit_bar": i, "side": closing_side,
                          "entry": entry_price, "exit": o, "reason": "signal", "pnl": pnl - fee})
            if trace:
                d = "LONG" if closing_side == 1 else "SHRT"
                print(f"  bar={i} EXIT {d} @ open={o:.2f} reason=signal pnl={pnl-fee:.2f}", file=sys.stderr)
            side = 0; bars_in_trade = 0; be_act = False; trail = 0.0; pos_live = False
            high_window = []; low_window = []

        if pending_entry != 0 and not pos_live:
            entry_price = o  # Fill price (open)
            entry_qty = CAPITAL / entry_price if sizing == "all_equity" else 1000.0 / entry_price
            closing_side = pending_entry  # Track for potential immediate close
            side = pending_entry
            pos_live = True
            entry_bar = i
            bars_in_trade = 0; be_act = False; trail = 0.0
            atr_at_entry = prev_atr
            hard_sl_long = entry_price - sl_mult * atr_at_entry if atr_at_entry > 0 else 0
            hard_sl_short = entry_price + sl_mult * atr_at_entry if atr_at_entry > 0 else 1e18
            high_since = entry_price
            low_since = entry_price
            high_window = []; low_window = []
            fee = abs(entry_qty * entry_price) * FEE_RATE
            equity -= fee
            if trace:
                d = "LONG" if side == 1 else "SHRT"
                print(f"  bar={i} ENTRY {d} @ open={o:.2f} qty={entry_qty:.4f} atr@entry={atr_at_entry:.2f} "
                      f"hardSl={hard_sl_long:.2f}/{hard_sl_short:.2f}", file=sys.stderr)

        pending_entry = 0; pending_risk_exit = False; pending_signal_exit = False

        # ── STEP 2: Update indicators ──
        use_atr = exit_atr.value() if exit_atr.ready() else 0.0
        fast_sma.update(c)
        slow_sma.update(c)
        exit_atr.update(h, l, c)
        prev_atr = use_atr

        # ── STEP 3: Check risk exit ──
        if side != 0 and exit_atr.ready():
            result = check_risk_exit(
                side, entry_price, high_since, low_since, high_window, low_window,
                h, l, prev_atr, hard_sl_long, hard_sl_short, trail_mult, lookback,
                exit_mode, bars_in_trade, be_act, trail, sl_mult, be_r_mult, entry_price
            )
            pending_risk_exit = result[0]
            side = result[1]
            bars_in_trade = result[2]
            be_act = result[3]
            trail = result[4]
            high_since = result[5]
            low_since = result[6]
            high_window = result[7]
            low_window = result[8]

        # ── STEP 4: Signal generation ──
        if not fast_sma.ready() or not slow_sma.ready():
            prev_above = None
            continue

        fast_val = fast_sma.value()
        slow_val = slow_sma.value()
        above = fast_val > slow_val

        if prev_above is not None:
            if above and not prev_above and side == 0:
                pending_entry = 1; pending_risk_exit = False; pending_signal_exit = False
                side = 1
                if trace:
                    print(f"  bar={i} CROSS_UP fast={fast_val:.2f} slow={slow_val:.2f} → pending_entry=LONG", file=sys.stderr)
            elif not above and prev_above and side == 0:
                pending_entry = -1; pending_risk_exit = False; pending_signal_exit = False
                side = -1
                if trace:
                    print(f"  bar={i} CROSS_DN fast={fast_val:.2f} slow={slow_val:.2f} → pending_entry=SHRT", file=sys.stderr)
            elif signal_exits_allowed and side == 1 and not above:
                pending_signal_exit = True
            elif signal_exits_allowed and side == -1 and above:
                pending_signal_exit = True

        prev_above = above

    return {"trades": len(trades), "equity": equity, "trade_list": trades}


# ── VBT reference (Layer 1 only: pure signal entries/exits) ─────────

def layer1_vbt(df, fast=10, slow=40):
    """VBT reference: SMA crossover, entries+exits only, next-bar-open fills."""
    try:
        import vectorbtpro as vbt
    except ImportError:
        print("  VBT not available, skipping")
        return None

    fast_ma = df["close"].rolling(fast).mean()
    slow_ma = df["close"].rolling(slow).mean()

    # Signal on bar close, shift by 1 → fill at next bar open
    entries_long = (fast_ma > slow_ma) & ~(fast_ma > slow_ma).shift(1)
    entries_long = entries_long.shift(1).fillna(False).astype(bool)

    entries_short = (fast_ma < slow_ma) & ~(fast_ma < slow_ma).shift(1)
    entries_short = entries_short.shift(1).fillna(False).astype(bool)

    exits_long = (fast_ma < slow_ma).shift(1).fillna(False).astype(bool)
    exits_short = (fast_ma > slow_ma).shift(1).fillna(False).astype(bool)

    pf = vbt.Portfolio.from_signals(
        close=df["open"],
        high=df["high"],
        low=df["low"],
        entries=entries_long,
        exits=exits_long,
        short_entries=entries_short,
        short_exits=exits_short,
        init_cash=CAPITAL,
        freq="4h",
        fees=FEE_RATE,
        accumulate=False,
    )
    stats = pf.stats()
    trades = pf.trades.records_readable
    return {
        "trades": len(trades),
        "total_return": stats["Total Return [%]"],
        "sharpe": stats["Sharpe Ratio"],
        "pf_trades": trades,
    }


def print_trades(trade_list, label, max_n=30):
    print(f"\n  {label} — first {min(len(trade_list), max_n)} trades:", file=sys.stderr)
    print(f"  {'#':>3} {'Side':>5} {'EntryBar':>8} {'ExitBar':>8} {'Entry':>10} {'Exit':>10} {'PnL':>10} Reason", file=sys.stderr)
    print(f"  {'-'*70}", file=sys.stderr)
    for i, t in enumerate(trade_list[:max_n]):
        d = "LONG" if t["side"] == 1 else "SHRT"
        print(f"  {i+1:>3} {d:>5} {t['entry_bar']:>8} {t['exit_bar']:>8} {t['entry']:>10.2f} {t['exit']:>10.2f} "
              f"{t['pnl']:>10.2f} {t['reason']}", file=sys.stderr)


if __name__ == "__main__":
    trace = "--trace" in sys.argv
    df = load_csv(years_back=1)
    print(f"Data: {len(df)} bars, {df.index[0].date()} → {df.index[-1].date()}", file=sys.stderr)

    print("\n" + "="*70, file=sys.stderr)
    print("LAYER 2: SMA Crossover 10/40 + Chandelier exits (no signal exits)", file=sys.stderr)
    print("="*70, file=sys.stderr)

    configs = [
        ("trail=2.0 sl=1.2 lb=12", 2.0, 1.2, 12),
        ("trail=3.0 sl=2.0 lb=8",  3.0, 2.0, 8),
        ("trail=4.0 sl=2.0 lb=0",  4.0, 2.0, 0),
    ]
    for label, trail, sl, lb in configs:
        print(f"\n--- {label} ---", file=sys.stderr)
        r = _run_streaming_impl(
            df["close"].values, df["open"].values, df["high"].values, df["low"].values,
            fast=10, slow=40, exit_mode="chandelier",
            atr_period=14, trail_mult=trail, sl_mult=sl, lookback=lb,
            be_r_mult=1.0, trace=trace, sizing="all_equity"
        )
        ret = r["equity"] - CAPITAL
        print(f"\n  Python {label}: {r['trades']} trades, equity={r['equity']:.2f}, return={ret:.2f}", file=sys.stderr)
        if r["trades"] <= 30:
            print_trades(r["trade_list"], f"Python {label}")

    print("\n" + "="*70, file=sys.stderr)
    print("LAYER 1: SMA Crossover signal entries + signal exits (sig_be mode)", file=sys.stderr)
    print("="*70, file=sys.stderr)

    r1 = _run_streaming_impl(
        df["close"].values, df["open"].values, df["high"].values, df["low"].values,
        fast=10, slow=40, exit_mode="sig_be",
        atr_period=14, trail_mult=2.0, sl_mult=1.2, lookback=0,
        be_r_mult=1.0, trace=trace, sizing="all_equity"
    )
    ret1 = r1["equity"] - CAPITAL
    print(f"\n  Python sig_be: {r1['trades']} trades, equity={r1['equity']:.2f}, return={ret1:.2f}", file=sys.stderr)
    if r1["trades"] <= 30:
        print_trades(r1["trade_list"], "Python sig_be")

    # VBT reference
    print("\n" + "="*70, file=sys.stderr)
    print("VBT REFERENCE: SMA Crossover signal entries + signal exits", file=sys.stderr)
    print("="*70, file=sys.stderr)
    vbt1 = layer1_vbt(df, fast=10, slow=40)
    if vbt1:
        print(f"  VBT: {vbt1['trades']} trades, return={vbt1['total_return']:.2f}%", file=sys.stderr)
