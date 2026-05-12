# Strategy Parity: VBT (Python) ↔ C++ (FLOX)

## What Achieved Parity

Three bugs stood between us and a 1:1 match. After fixing them, all 3 test configs produced
**identical trade counts, entry bars, exit bars, sides, and fill prices** bar-by-bar.

### Bug 1: Entry Price Used Close Instead of Open (C++)

**Location**: `src/main.cpp` STEP 1 (pending execution block)

**Root cause**: When executing a pending entry, the code used `c` (real close, stored in the
`open` field after the swap) for `entryPrice_`, `highSince_`, `lowSince_`, and `qtyForTrade()`.
But the actual fill happens at the **open price** of the next bar — which is `fillPrice`
(stored in the `close` field after the swap).

**Impact**: Wrong hard SL levels (`entryPrice_ - sl_mult * atr` computed from wrong price),
wrong trail anchors (`highSince_` initialized to wrong value), wrong qty. This caused
different trades to trigger risk exits at different times, cascading into entirely different
trade sequences.

**Fix**:
```cpp
// BEFORE (wrong):
emitMarketBuy(sym, qtyForTrade(c, sizing_));
entryPrice_ = c;  highSince_ = c;  lowSince_ = c;

// AFTER (correct):
emitMarketBuy(sym, qtyForTrade(fillPrice, sizing_));
entryPrice_ = fillPrice;  highSince_ = fillPrice;  lowSince_ = fillPrice;
```

**Why it was hard to spot**: The variable was named `c` suggesting "current price" but `c`
was actually the real close (for indicator computation), not the fill price. The open↔close
swap in `csvToBarEvents()` made this confusing.

### Bug 2: prevAbove Defaulted to false (C++)

**Location**: `SmaCrossStrat` and `EmaCrossStrat`

**Root cause**: `bool prevAbove_ = false` means on the very first bar where SMAs become ready,
`above=true, prevAbove_=false` triggers a false crossover signal. Python correctly uses
`prev_above = None` and skips signal generation until there's a real previous value to compare.

**Impact**: One phantom trade at the very start of the data (bar 40 in our test), causing
cascading misalignment in all subsequent trades.

**Fix**: Added `bool prevAboveValid_ = false` flag. Signal generation only runs when
`prevAboveValid_` is true, which is set after the first ready bar.

### Bug 3: Short-Side Chandelier Reused high_window (Python)

**Location**: `vbt_parity.py` layer2_streaming, short-side risk exit

**Root cause**: For short positions, the chandelier anchor should use the **lowest low** in the
lookback window (`low_window` / `low_since`). But the code reused `high_window` and
`high_since` for both long and short tracking.

**Impact**: Wrong trail levels for short trades, causing different exit timing. Also the
short hard_sl formula used the long formula (`entry - sl*atr`) instead of
`entry + sl*atr`.

**Fix**: Separate `low_window` / `low_since` for shorts, matching C++ which already had
`highWindow_` / `lowWindow_` as separate deques.

---

## The Debugging Methodology That Worked

### Step 1: Start With the Simplest Strategy

SMA crossover is the canonical test: two moving averages, unambiguous crossover signals,
no ambiguous boundary conditions. Don't test Donchian or Supertrend for initial parity —
their signals depend on rolling max/min windows which add complexity.

### Step 2: Start With the Simplest Exit Mode

**Chandelier** is the best first exit mode for parity because:
- **No signal exits** (only risk exits), so you don't need to align signal exit logic
- The trail/SL logic is deterministic given price, ATR, and anchor
- It exercises the full pending mechanism (1-bar shift)

### Step 3: Add Bar-Level Trace Logging to C++

```
bar=75 CROSS_DN fast=103217.03 slow=103365.06 → pending_entry=SHRT
bar=75 ENTRY SHRT @ open=102335.30 qty=0.0977 atr@entry=1637.03 hardSl=104299.74
bar=76 CHAN_SHRT bits=1 anchor=102335.30 trail=105513.45 hardSl=104299.74 level=104299.74 high=103242.00 hold
bar=79 EXIT SHRT @ open=103979.10 reason=risk
```

### Step 4: Match First Trade Bar-by-Bar

Compare Python and C++ outputs. The **first divergence point** reveals the bug.
In our case:
- Python first trade: bar=75 (no bar=40 entry)
- C++ first trade: bar=40 (phantom crossover) → found Bug 2

### Step 5: Match All Trades

After fixing, compare the full trade list (entry bars, exit bars, prices, sides).
Any remaining divergence means there's still a logic difference.

---

## Exit Modes: Difficulty Order for Parity

### Tier 1: Chandelier (START HERE)

- Only risk exits, no signal exits
- Exercises: ATR shift, frozen SL, lookback cap, pending mechanism
- Deterministic: given same ATR + price → same exit level

### Tier 2: Chandelier + Time Stop

- Adds time-based forced exit (bars_in_trade >= threshold)
- Tests: barsInTrade_ counter alignment
- Same as Chandelier but with one additional exit condition

### Tier 3: Signal + BE Trail (HARDEST)

- Has BOTH signal exits AND risk exits
- Risk exits use BE activation + trailing stop (stateful)
- Tests: interaction between signal exit requests and risk exit checks
- BE activation depends on price reaching entry + be_r_mult * initial_range
- Trail updates every bar after BE activation
- Most complex state management: beAct_, trail_, hardSl all interact

### Which Exit Mode to Choose for Parity Testing

| Phase | Mode | Why |
|-------|------|-----|
| Initial parity | Chandelier | Simplest, deterministic |
| After parity | ChandelierTimeStop | Tests counter alignment |
| Final validation | SignalBETrail | Full state machine |

---

## Checklist: Adding a New Strategy

### 1. Implement the Signal in C++

Add a new class in `src/main.cpp` inheriting from `BaseStrategy`:

```cpp
class MyNewStrat final : public BaseStrategy {
    StreamSma param1_;  // or StreamEma, StreamRsi, etc.
public:
    MyNewStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r,
               const Params& p, SizingMode sz = SizingMode::FixedNotional)
        : BaseStrategy(sid, s, r, p, sz), param1_(p.signal_p1) {}
    void onBarImpl(double h, double l, double c) override {
        param1_.update(c);
        if (!param1_.ready()) return;
        // ... signal logic ...
        SymbolId sym = symbol();
        if (entry_condition && side_ == 0) enterLong(sym, c);
        else if (entry_condition && side_ == 0) enterShort(sym, c);
        else if (signalExitsAllowed() && side_ == 1 && exit_condition) requestExit();
        else if (signalExitsAllowed() && side_ == -1 && exit_condition) requestExit();
    }
};
```

### 2. Add to Factory

In `allStrats()`:
```cpp
{"my_new_strat", [](auto sid, auto s, auto& r, auto& p, auto sz) {
    return std::make_unique<MyNewStrat>(sid, s, r, p, sz);
}},
```

### 3. Add Grid Config

In `grids` vector:
```cpp
{"my_new_strat", {10,20,30}, {0}, {0}},  // p1 values, p2, p3
```

### 4. Implement Matching Python Streaming

In the parity script, create a function that mirrors the C++ signal logic exactly.
Use the same indicator classes (`StreamSma`, `StreamAtr`, etc.).

### 5. Verify Parity Checklist

- [ ] **Warmup alignment**: Both engines produce first signal on the same bar
- [ ] **Entry bar match**: First entry occurs on the same bar
- [ ] **Entry price match**: Both fill at the same price (next-bar open)
- [ ] **Exit bar match**: First exit occurs on the same bar
- [ ] **Exit price match**: Both exit at the same price
- [ ] **All trades match**: Full trade list (bars, prices, sides) identical
- [ ] **Multiple configs**: Test with at least 2-3 different param combos
- [ ] **Multiple exit modes**: Chandelier passes, then test SignalBETrail

---

## Things to Watch Out For

### 1. Open↔Close Swap

`csvToBarEvents()` swaps `open` and `close` so FLOX fills at the real open price.
In `onSymbolBar()`:
- `c = ev.bar.open` → **real close** (stored in open field)
- `fillPrice = ev.bar.close` → **real open** (stored in close field)
- `h = ev.bar.high` → real high (unchanged)
- `l = ev.bar.low` → real low (unchanged)

**Rule**: Use `c` for indicator computation, `fillPrice` for entry/exit prices and qty.

### 2. Pending Mechanism (1-Bar Shift)

Signal detected on bar N → fills at bar N+1 open.

The pending mechanism works like this:
```
Bar N:
  1. Execute pending actions from bar N-1 (fills at bar N open)
  2. Update indicators
  3. Check risk exits → set pendingRiskExit_ for bar N+1
  4. Generate signal → set pendingEntry_ for bar N+1

Bar N+1:
  1. Execute pending actions from bar N (fills at bar N+1 open)
```

**Critical**: `side_` is set immediately in `enterLong()`/`enterShort()` (before the
pending entry executes), so signal logic on subsequent bars can check `side_` to know
a pending entry exists. But `posLive_` is only set when the pending entry actually fills.

### 3. ATR Shift

`prevAtr_` captures the ATR value BEFORE the update, matching Trader7's `atr.shift(1)`:
```cpp
double useAtr = exitAtr_.ready() ? exitAtr_.value() : 0;
exitAtr_.update(h, l, c);
prevAtr_ = useAtr;  // Used for stop computation on NEXT bar
```

### 4. Frozen SL at Entry

`atrAtEntry_` is captured when the pending entry fills (at open price), not when the
signal is generated. Hard SL levels are computed once and never change:
```cpp
hardSlLong_ = entryPrice_ - p_.exit_sl_mult * atrAtEntry_;
hardSlShort_ = entryPrice_ + p_.exit_sl_mult * atrAtEntry_;
```

### 5. Chandelier Lookback Cap

`highWindow_` / `lowWindow_` deques are capped at `exit_lookback` bars.
The anchor is computed BEFORE pushing the current bar (excludes bar i):
```cpp
double anchor = *std::max_element(highWindow_.begin(), highWindow_.end());
highWindow_.push_back(h);
if (p_.exit_lookback > 0 && int(highWindow_.size()) > p_.exit_lookback)
    highWindow_.pop_front();
```

### 6. Signal Exit Suppression

In chandelier modes, `signalExitsAllowed()` returns false. Only risk exits fire.
This is critical — if signal exits were allowed, the pending mechanism would create
extra trades from crossovers that happen inside a risk-managed position.

### 7. EMA vs SMA Warmup

- `StreamSma::ready()` returns true when `buf.size() >= period`
- `StreamEma::ready()` returns true when `n_ >= 1` (after one update past seed)
- `StreamAtr::ready()` returns true when `n_ >= period`

This matters for strategies using EMA (EmaCross, Keltner, Supertrend) — the first
signal appears much earlier than SMA-based strategies.

### 8. Side Tracking for Debug Display

`side_` is cleared to 0 by `checkRiskExit()` when a risk exit is triggered. So when
STEP 1 prints the EXIT message, `side_` is already 0. Use `exitSide_` to track the
correct side for display purposes.

---

## File Layout

```
vendor/flox/_my/crush-glm/
├── src/main.cpp          # C++ engine (strategies, grid, WFO, WRC)
├── vbt_parity.py         # Python streaming parity reference
├── data/                 # CSV files (BTCUSDTUSDT_4h.csv, etc.)
├── results/              # Grid output (grid_summary.csv, grid_all_combos.csv)
└── docs-ai/
    └── STRATEGY_PARITY.md  # This file
```

## Quick Commands

```bash
# Build C++
cd vendor/flox/_my/crush-glm
cmake --build build -j$(sysctl -n hw.ncpu)

# Run debug parity (3 configs, no trace)
./build/crush_grid --symbol BTCUSDT --years-back 1 --sizing all_equity --min-trades 5 --output results/btc_debug --debug

# Run debug with full trace (single config)
# Change the debug block in main.cpp to trace=true, rebuild, redirect stderr

# Run Python parity
cd /Users/lex/WorkspaceTrading/Trader7-Kilo
python3 vendor/flox/_my/crush-glm/vbt_parity.py --trace 2>&1 | head -100

# Run full grid
./build/crush_grid --symbol BTCUSDT --years-back 1 --sizing all_equity --min-trades 5 --output results/btc_v8
```
