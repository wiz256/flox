# WFO, Grid Search, and Real-World Alpha Discovery Workflows

## Part 1: Your WFO Question

### The Core Question

> If we use the same grid params anyways (preconfigured), what changes if we test on
> full 2 years vs 2y-3m + last 3m? And if I want to weight recent data MORE, not less?

### Answer: It Depends On What You're Optimizing

There are two fundamentally different questions you can ask:

**Question A: "Does combo X work?"** (Performance estimation)
→ Test combo X on the full 2 years. Done. No WFO needed. You get the PnL, Sharpe,
   drawdown for the full period. You can slice by trade open/exit time to see how it
   performed in each 3-month window.

**Question B: "Would I have FOUND combo X without looking at the future?"** (Overfitting detection)
→ This is where WFO matters. If you run 500 combos on 2 years and pick the best,
   the question is: would the same combo also be the best if you only had 18 months?
   If different combos win on different sub-periods, your "best" is just the one that
   happened to fit the full period's quirks.

### When WFO Matters vs When It Doesn't

| Scenario | WFO Needed? | Why |
|----------|:-----------:|-----|
| You test 5 combos on 2 years | No | Only 5 tests → low multiple-testing burden |
| You test 500 combos on 2 years | Yes | Picking best-of-500 is almost certainly overfit |
| You test 500 combos but use CSCV/PBO | Alternative | PBO directly measures overfitting probability |
| You want to weight recent data more | No (different approach) | Use recency-weighted scoring instead |

### Your Recency Weighting Idea Is Valid

Wanting to weight recent performance more is a legitimate approach called **adaptive
optimization** or **rolling window optimization**. It's what many live trading desks
actually do:

```
score = 0.3 * sharpe(full_2y) + 0.7 * sharpe(last_6m)
```

This is NOT WFO — it's a different (and often better) approach for live trading.
WFO tests robustness across time. Recency weighting bets that recent regimes persist.

### Can You Just Analyze Trade-Level Performance Every 3m?

**Yes, absolutely.** If you have a grid of combos and test each on the full 2 years,
you can absolutely break down each combo's performance by 3-month windows:

```
combo_A: Q1=+2.1%  Q2=-0.5%  Q3=+1.8%  Q4=+3.2%  Q5=+0.9%  Q6=+1.5%  Q7=-1.2%  Q8=+2.0%
combo_B: Q1=+5.0%  Q2=-8.2%  Q3=+4.1%  Q4=-6.3%  Q5=+3.8%  Q6=-7.1%  Q7=+4.5%  Q8=-5.9%
```

Combo A is better even though combo B has higher absolute returns — combo B is
inconsistent (high variance across windows), combo A is steady (low variance).

This is exactly what **stability scoring** does. You don't need WFO for this.

### The Real Purpose of WFO (Simplified)

WFO answers ONE specific question: "If I re-optimized every N months, would I pick
different params each time?" If yes → the edge is regime-dependent. If no → the edge
is structural and persistent.

For your pipeline (preconfigured grid, test all combos, pick best):
- **Without WFO**: You find the best combo on 2y and hope it keeps working
- **With WFO**: You check if that combo also worked on sub-periods it wasn't "trained" on
- **With CSCV/PBO**: You check if the ranking of combos is stable across random sub-splits

All three are valid. CSCV/PBO is often more informative than WFO for your use case.

---

## Part 2: Real-World Quant Alpha Discovery Workflows

### Ranked From Most to Least Promising

---

### Workflow 1: Grid Search + Overfitting Defense (Most Robust)

**What it is**: Exhaustive grid → CSCV/PBO overfitting check → plateau detection → stress testing.

**How it works**:
1. Define parameter bounds based on domain knowledge
2. Run dense grid (500-5000 combos)
3. For each combo: compute Sharpe, total return, max DD, trade count
4. Compute PBO via CSCV: randomly split data into S subsets, train on all combinations
   of S-1 subsets, test on the held-out one. PBO = probability the best IS combo is
   also best OOS. High PBO → overfitting.
5. Plateau detection: the best combo's neighbors should also score well. If only one
   isolated point scores well → spike (noise). If a flat region scores well → plateau (signal).
6. Stress test: add slippage, fees, random delays. Does the edge survive?

**Pros**:
- Catches overfitting directly (PBO measures it)
- Plateau detection separates signal from noise
- No "training" in the traditional sense — you test everything
- Works well with moderate parameter spaces (2-4 params)

**Cons**:
- Computationally expensive for large grids
- PBO assumes stationarity of the data splits
- Doesn't adapt to regime changes

**When to use**: Default approach. Use this when you have 2-5 parameters and domain
knowledge about reasonable ranges.

**Who uses this**: Most systematic trading firms. This is the standard workflow for
parameterized strategies.

---

### Workflow 2: Optuna/TPE + Stability Probing

**What it is**: Smart search (not exhaustive) + perturbation stability + plateau validation.

**How it works**:
1. Use Optuna's TPE sampler to explore parameter space efficiently
2. Each trial gets a Sharpe score from a backtest
3. Top 50 trials get perturbation tests: ±5% on each parameter, re-run
4. Low variance across perturbations → genuine plateau
5. High variance → spike, discard
6. Extract KDE (kernel density estimate) of top-performing regions
7. Run dense grid ONLY in the identified plateau regions

**Pros**:
- 10-50x faster than full grid for high-dimensional spaces
- Natural plateau detection through perturbation
- Can handle 5-10 parameters efficiently
- KDE zone extraction finds multi-modal regions

**Cons**:
- TPE is exploitation-focused — may miss distant plateaus
- Needs enough initial random trials to seed properly
- Perturbation stability doesn't catch all forms of overfitting

**When to use**: High-dimensional parameter spaces (5+ params), limited compute,
or as a pre-filter before dense grid.

**Who uses this**: Optuna is used by JP Morgan, Nomura, and many crypto quant shops.
The perturbation approach is from Bailey et al. "The Probability of Backtest Overfitting".

**This is what Trader7's RANGEFIND phase does.**

---

### Workflow 3: Walk-Forward Optimization (Traditional)

**What it is**: Sequential train/test splits with rolling windows.

**How it works**:
1. Split data into N folds (e.g., 5 folds of 5 months each)
2. For each fold: train on the fold (find best params), test on the next fold
3. Average OOS performance → estimate of live performance
4. OOS/IS ratio < 0.6 → overfit

**Pros**:
- Directly answers "would this have worked in real-time?"
- Models the actual deployment workflow (optimize → deploy → re-optimize)
- Simple to understand and implement

**Cons**:
- Only uses each data point once for OOS → high variance in estimate
- Sensitive to fold boundaries (a lucky boundary can inflate results)
- "Training" in WFO is often just finding the best single combo → same overfitting risk
- Low statistical power for short datasets

**When to use**: When you want to simulate a realistic deployment schedule. Good for
strategies that you plan to re-optimize monthly/quarterly.

**Who uses this**: Retail traders, some fund desks that re-optimize on a schedule.
Less common in sophisticated shops — CPCV (Workflow 4) is preferred.

---

### Workflow 4: Combinatorial Purged Cross-Validation (CPCV)

**What it is**: López de Prado's method. All possible train/test splits with purging.

**How it works**:
1. Split data into N groups (e.g., 8 groups of 3 months)
2. For each combination of choosing k groups for training (C(N,k) combinations):
   - Train on k groups → find best params
   - Test on remaining N-k groups
3. Average across ALL combinations → unbiased performance estimate
4. "Purge" the gap between train and test to prevent leakage

**Pros**:
- Uses all data for both training and testing (every point is OOS in some splits)
- Much lower variance than traditional WFO
- Purging prevents information leakage across train/test boundaries
- Theoretically sound (comes with mathematical guarantees)

**Cons**:
- Computationally brutal: C(16,8) = 12,870 combinations
- Each combination requires a full optimization → very expensive
- Complex to implement correctly (purging, embargo)
- Overkill for small parameter spaces

**When to use**: When you have lots of compute and need the most rigorous overfitting
test possible. Gold standard for publication or regulatory review.

**Who uses this**: Quant funds following López de Prado's "Advances in Financial Machine
Learning". Initially developed for Abeomics/QuantConnect.

---

### Workflow 5: Rolling Ensemble (Adaptive)

**What it is**: Don't pick one best combo — use an ensemble that adapts over time.

**How it works**:
1. Run full grid on trailing 12-month window
2. Take top-5 combos (not just the best)
3. Average their signals (ensemble)
4. Every month: re-run grid on the new trailing 12-month window
5. Update ensemble weights (more recent performance → higher weight)

**Pros**:
- Adapts to regime changes naturally
- Ensemble is more robust than any single combo
- Recent data gets more weight (your preference)
- Reduces the "which combo to pick" problem

**Cons**:
- Always slightly behind the current regime (trailing window)
- Higher turnover (ensemble changes monthly)
- More complex to implement and monitor
- Can ensemble together combos that contradict each other

**When to use**: Live trading with regular re-optimization. Best when you believe
regimes change every 6-18 months.

**Who uses this**: Many CTAs and systematic macro funds. AQR's style factors use
rolling optimization windows.

---

### Workflow 6: Monte Carlo Stress Testing

**What it is**: Don't test different params — test different realities.

**How it works**:
1. Find best combo on actual data
2. Randomly shuffle trade returns 1000 times
3. Check: is 5th-percentile PnL still positive?
4. Add random slippage delays (0-3 bars)
5. Test with 2x, 3x, 5x fees
6. Check: does the edge survive 95% of alternate realities?

**Pros**:
- Tests robustness without needing different params
- Catches "lucky trade ordering" (same trades, different sequence)
- Simple to implement and interpret
- Works with any optimization method

**Cons**:
- Assumes trade returns are i.i.d. (they're often autocorrelated)
- Doesn't catch overfitting in parameter selection
- Slippage simulation is crude (real slippage is path-dependent)

**When to use**: As a final validation step after any optimization method. Always
run this before deploying.

**Who uses this**: Everyone. This is table stakes.

---

### Workflow 7: Pure Signal Research (No Optimization)

**What it is**: Don't optimize parameters at all — use economic rationale.

**How it works**:
1. Identify a market inefficiency through domain knowledge
2. Design a signal that exploits it with fixed, reasoned parameters
3. Backtest once. If it works → deploy. If not → rethink the thesis.
4. No parameter search → zero overfitting risk

**Pros**:
- Zero overfitting risk (no optimization = no multiple testing)
- Economically grounded (you understand WHY it works)
- Simple, fast to implement
- Parameters have economic meaning (e.g., 200-day MA because institutions use it)

**Cons**:
- Requires deep market understanding
- You might miss better parameter values
- Very few genuine edges are discoverable this way
- Confirmation bias risk (you rationalize why it should work)

**When to use**: When you have genuine market insight. Best for fundamentally-driven
strategies (carry, value, momentum).

**Who uses this**: Renaissance (reportedly), some discretionary quant funds, academic
researchers testing specific hypotheses.

---

## Part 3: What Pro Quants Actually Do (Realistic)

### The Honest Answer

Most quant shops use **Workflow 1 (Grid + Overfitting Defense)** as the primary method,
with **Workflow 6 (Monte Carlo)** as validation. Here's the typical pipeline:

```
1. Domain research → identify candidate strategy
2. GRID SEARCH on 2-4 parameters (100-2000 combos)
3. CSCV/PBO check → discard if PBO > threshold
4. Plateau detection → discard if isolated spike
5. Slippage stress test → discard if edge vanishes with realistic costs
6. Monte Carlo shuffle → discard if 5th-pctile PnL < 0
7. Paper trade 30-90 days → discard if live ≠ backtest
8. Deploy with circuit breakers
```

### The "WFO With Random Params" Misconception

No one runs WFO with random params. WFO is used WITH grid search:

```
WRONG:   Generate 100 random param combos → WFO → deploy best
RIGHT:   Grid search 1000 combos → pick top 5 → WFO on each → deploy survivor
```

WFO doesn't find params. It validates that the params you found aren't overfit.

### Optuna's Role

Optuna is used for **efficient search**, not as a replacement for overfitting checks:

```
1. Optuna TPE: 500 trials → find 3 promising regions
2. Dense grid in those regions: 200 combos each
3. CSCV/PBO on all 600 combos → check overfitting
4. Plateau detection in surviving combos
5. Stress test survivors
```

The key insight: Optuna finds candidates faster, but you still need the same
overfitting defense. Optuna doesn't replace CSCV/PBO — it precedes it.

### How Plateaus Prevent Noise Spikes

```
Parameter Space: window=[10, 200]

Noise spike:  Only window=143 works, window=142 and 144 are terrible
              → overfit to specific data pattern at exactly 143
              → PBO will catch this (different splits → different "best" window)

Plateau:      window=[120, 160] all score well
              → genuine edge that's robust to exact parameter choice
              → PBO will confirm (same region wins across splits)
```

### Why Pros Don't Worry About WFO "Training" Period

Because they're not "training" in the ML sense. The grid search tests ALL combos on
ALL data. There's no iterative learning that could overfit. The overfitting risk is
from **multiple testing** (testing many combos and picking the best), which is what
PBO/CSCV directly measures.

WFO is a cruder version of CSCV that's easier to implement but less statistically
rigorous. It's fine for a quick check but shouldn't be your primary defense.

---

## Part 4: Comparison Table

| Method | Speed | Overfitting Defense | Best For | Complexity |
|--------|-------|-------------------|----------|------------|
| Grid + CSCV/PBO | Slow | **Best** (direct measurement) | 2-4 params, all strategies | Medium |
| Optuna + Perturbation | Fast | Good (stability check) | 5+ params, large spaces | Medium |
| WFO | Medium | OK (but noisy estimate) | Sequential re-optimization | Low |
| CPCV | Very slow | **Best** (theoretical) | Regulatory/publication | High |
| Rolling Ensemble | Medium | Good (diversification) | Live trading, regime adaptation | High |
| Monte Carlo | Fast | Good (robustness) | Final validation | Low |
| Pure Signal | Fastest | **Best** (no optimization) | Genuine market insight | Low |

### Recommended Workflow for Your Pipeline

Since you already have RANGEFIND (Optuna) → GRIDWALK (grid + plateau) → PROVE (7 tests):

1. **RANGEFIND**: Already correct (TPE + Sobol + CMA-ES → perturbation → KDE zones)
2. **GRIDWALK**: Already correct (dense grid in zones → PBO/CSCV → plateau detection)
3. **PROVE**: Already correct (7 independent tests attacking different failure modes)
4. **FIELD**: Already correct (paper trading with circuit breakers)

Your pipeline is actually better than what most shops use. The WFO in your PROVE phase
is just one of 7 tests — it's not the primary defense (PBO/CSCV in GRIDWALK is).

### If You Want Recency Weighting

Add it as a scoring modifier in GRIDWALK:

```python
# In dense_sweep scoring:
full_sharpe = backtest(all_bars, combo)
recent_sharpe = backtest(last_6m_bars, combo)
score = 0.4 * full_sharpe + 0.6 * recent_sharpe
```

This naturally gives more weight to recent performance without needing WFO.
