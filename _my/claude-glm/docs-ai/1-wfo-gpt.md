https://chatgpt.com/c/6a00d35b-1850-83eb-b8db-4765989f205c

QUESTION:

algo trading. please explain me best practices of how WFO (walk forward) is done. it's concept, best practices. how many folds should we use. how to avoid lookahead biases and other issues. which common bugs happen and how to avoid it. how should ideal WFO look like. Why beside WFO quants recommend also to do Monte Carlo, Roman Woflf, DSO/DSR, and what else and why? explain all robustness teqniques. why WFO is not enough?

Right now I do WFO on past 2 years data on H4 timeframe with 5 folds.
train bars: 613
test bars: 239
periods:
2024-05-09 - 2024-08-19 (train)  / 2024-08-23 - 2024-10-02 (test)
2024-10-02 - 2025-01-12 (train)  / 2025-01-16 - 2025-02-25 (test)
2025-02-25 - 2025-06-07 (train)  / 2025-06-11 - 2025-07-21 (test)
2025-07-21 - 2025-10-31 (train)  / 2025-11-04 - 2025-12-14 (test)
2025-12-14 - 2026-03-26 (train)  / 2026-03-30 - 2026-05-09 (test)
My trasholds: OOS >= 5 trades, IS >= 20 trades. - is that good trasholds? 
and even with this strasholds, almost all of them fail because of insuficient trades count!

which issues do you see?
should we do less folds than 5? how many folds minimum needed?

I understand between train and test on the same fold we must make some gap in time to avoid lookahead bias
BUT do we need the next fold's training period start to be the end of the previous fold end of test period? if we do 5 folds on 2 years, the periods become very small and we don't have enough trades to do WFO correctly! is it allowed (and if it's a good practice) that we would still do 5 folds, but we change the training periods and testing periods in a way that next folds training period is, let's say, in the middle of the previous folds training period, but testing period NEVER overlaps with the previous testing period? maybe something different, but you got the idea, my idea is to allow much MORE trades to be inside the folding period! or make less folding periods - and here I'm not sure what is better and what is worse. Do you understand what I want to achieve?

Please give me EVERYTHING I should know about WFO and other robustness and proving techniques for trading strategies!


---

ANSWER:

Yes, I understand exactly what you want to achieve: you are trying to keep **enough trades per fold** while still having a proper OOS test. Your current WFO is probably too strict in the wrong place and too weak in the important statistical places.

## 1. Core idea of WFO

Walk-forward optimization is meant to answer this:

> “If I only knew the past, optimized on the past, then traded the next unseen period, would the strategy keep working?”

A proper WFO loop is:

```text
For each fold:
  1. Take historical training window.
  2. Optimize/select parameters only on that training window.
  3. Freeze parameters.
  4. Trade the next unseen test window.
  5. Store OOS result.
Combine all OOS windows into one synthetic live-equity curve.
```

The most important rule:

> The test period must be completely unseen by the optimizer.

WFO is not just “split data into 5 chunks.” It is a simulation of repeated real-life re-optimization.

---

## 2. Your current WFO setup: main issues

You currently use:

```text
2 years of H4 data
5 folds
train bars: 613  ≈ 102 days
test bars: 239   ≈ 40 days
IS threshold: 20 trades
OOS threshold: 5 trades
```

### Main problem

For H4 crypto strategies, **613 training bars is very small**.

102 days of H4 candles is not enough for serious parameter optimization unless the strategy is very high-frequency. Your test window is also only around 40 days. For IFVG / breakout / trend / pullback strategies, that can easily produce too few trades.

Your current WFO mostly tests:

> “Can this strategy produce enough trades in tiny 40-day windows?”

Not:

> “Is this strategy robust across regimes?”

That is why almost everything fails on insufficient trades.

---

## 3. Are your trade-count thresholds good?

### `IS >= 20 trades`

For optimization, **20 trades is very low**.

It can be acceptable as a hard minimum to avoid totally empty folds, but it is not enough to trust optimization. With only 20 trades, one or two lucky trades can completely change PF, Sharpe, expectancy, drawdown, and win rate.

Better:

```text
Minimum IS trades:
- Bare minimum: 30
- Better: 50+
- Stronger: 100+
```

For low-frequency H4 strategies, you may not get 100 trades per pair. So you need either:

```text
more history
more pairs
larger training windows
less granular WFO
or aggregate strategy-family evidence across markets
```

### `OOS >= 5 trades`

Per fold, 5 trades is okay as a **minimum activity check**, but useless as proof.

A fold with 5 trades can tell you:

```text
The strategy was active.
```

It cannot tell you:

```text
The edge is statistically reliable.
```

Better approach:

```text
Per-fold OOS minimum:
- 3–5 trades: acceptable only as activity check

Total combined OOS across all folds:
- Bare minimum: 30 trades
- Better: 50–100 trades
- Stronger: 100+ trades
```

So I would not reject a strategy only because one fold has 4 trades. I would reject it if the **combined OOS** has too few trades or if the results are concentrated in one fold/pair.

---

## 4. Should you use fewer than 5 folds?

For your current dataset: probably yes.

With only 2 years of H4 data, 5 folds can be too many **if each fold becomes tiny**.

Good practical rule:

```text
Number of folds is less important than:
1. enough training data per fold
2. enough OOS trades in total
3. enough regime coverage
4. non-overlapping test periods
```

### Minimum folds

I would use:

```text
Minimum acceptable: 3 folds
Good practical range: 4–6 folds
More than 6: only if you have enough data/trades
```

For your exact case, I would prefer:

```text
3 folds with larger test windows
or
5 folds with overlapping training windows and non-overlapping test windows
```

---

## 5. Do next fold training periods need to start after the previous test?

No.

This is one of the key misunderstandings.

Your current folds are almost like independent chunks:

```text
train → test
then next train starts after previous test
```

That wastes a lot of useful historical data.

It is absolutely normal for training windows to overlap.

Example:

```text
Fold 1:
Train: Jan 2024 – Dec 2024
Test:  Jan 2025 – Feb 2025

Fold 2:
Train: Mar 2024 – Feb 2025
Test:  Mar 2025 – Apr 2025

Fold 3:
Train: May 2024 – Apr 2025
Test:  May 2025 – Jun 2025
```

The test windows should usually not overlap if you want to combine them into one clean OOS equity curve.

But training windows can overlap heavily.

That is not only allowed; it is usually the correct way.

---

## 6. Anchored vs rolling WFO

There are two common WFO styles.

### A. Rolling window WFO

```text
Train 12 months → Test 2 months
Move forward 2 months
Train next 12 months → Test next 2 months
```

Good when the market changes and old data may become less relevant.

Best for:

```text
crypto
intraday systems
regime-sensitive strategies
short-to-medium-term edges
```

### B. Anchored / expanding WFO

```text
Train Jan 2022 – Dec 2023 → Test Jan 2024 – Mar 2024
Train Jan 2022 – Mar 2024 → Test Apr 2024 – Jun 2024
Train Jan 2022 – Jun 2024 → Test Jul 2024 – Sep 2024
```

Good when more data improves stability and the strategy logic is expected to be persistent.

Best for:

```text
daily/H4 strategies
low-frequency strategies
cross-market strategies
slower alpha families
```

For your H4 strategies, I would test both, but I would trust **anchored/expanding WFO** more than your current tiny rolling chunks.

---

## 7. Better WFO design for your exact case

You have 2 years of H4 data. I would not use 102-day train / 40-day test as the main proof.

### Option A: 3-fold WFO, stronger windows

```text
Fold 1:
Train: 2024-05-09 – 2025-01-09
Test:  2025-01-13 – 2025-04-13

Fold 2:
Train: 2024-08-09 – 2025-04-09
Test:  2025-04-13 – 2025-07-13

Fold 3:
Train: 2024-11-09 – 2025-07-09
Test:  2025-07-13 – 2025-10-13
```

Then keep 2025-10 to 2026-05 as final untouched holdout, or use it as additional OOS only after strategy selection.

### Option B: 5 folds with overlapping train, non-overlapping test

```text
Train length: 9–12 months
Test length: 1.0–1.5 months
Step: test length
Embargo: a few H4 bars to several days, depending on max holding period
```

Example:

```text
Fold 1:
Train: 2024-05 – 2025-05
Test:  2025-05 – 2025-06

Fold 2:
Train: 2024-06 – 2025-06
Test:  2025-06 – 2025-07

Fold 3:
Train: 2024-07 – 2025-07
Test:  2025-07 – 2025-08

Fold 4:
Train: 2024-08 – 2025-08
Test:  2025-08 – 2025-09

Fold 5:
Train: 2024-09 – 2025-09
Test:  2025-09 – 2025-10
```

But with H4, this may still be trade-starved.

### Option C: better for low-frequency H4

Use more history.

```text
Train: 18–36 months
Test: 3–6 months
Folds: 4–8
```

For H4 crypto, this is much more realistic.

---

## 8. Important: WFO should be based on trades, not only bars

Bars are not the real sample size.

A strategy with 10,000 H4 bars but 25 trades still has a weak sample.

For trading validation, you care about:

```text
number of trades
number of independent trades
number of regimes
number of markets
number of parameter variants tested
```

For example:

```text
Strategy A:
5 years, 200 trades, 20 pairs, 8 regimes → useful

Strategy B:
5 years, 18 trades, 1 pair, one bull market → weak
```

So your WFO design should target enough **OOS trades**, not just folds.

---

## 9. What should a good WFO report show?

For each fold:

```text
fold_id
train_start
train_end
test_start
test_end
chosen_params
IS trades
OOS trades
IS PF
OOS PF
IS expectancy R
OOS expectancy R
IS max DD
OOS max DD
IS Sharpe
OOS Sharpe
OOS return
OOS drawdown
parameter stability score
rank of selected params in training
```

Across all folds:

```text
combined OOS equity curve
combined OOS PF
combined OOS expectancy
combined OOS Sharpe / Sortino
combined OOS max DD
median fold PF
worst fold PF
percentage of profitable folds
percentage of folds with PF > 1
OOS trades total
OOS trades per fold
parameter drift
market/regime attribution
```

For your strategy lab, I would score WFO like this:

```text
Pass:
- combined OOS PF > 1.20–1.30
- combined OOS expectancy > 0
- at least 30–50 combined OOS trades
- no single fold contributes more than 40–50% of profit
- at least 60% of folds profitable
- worst fold drawdown acceptable
- parameters are stable / plateau-like

Strong pass:
- combined OOS PF > 1.40
- 50–100+ OOS trades
- 70%+ folds profitable
- Monte Carlo lower bound > 0
- DSR/PSR acceptable
- survives costs/slippage stress
```

---

## 10. Lookahead bias in WFO

Lookahead bias happens when the strategy uses information that would not have existed at the time of the trade.

Common examples:

```text
Using future candles to confirm a signal.
Using the full dataset to normalize features.
Using future ATR/volatility values.
Using future swing highs/lows.
Using final daily high/low before the day is finished.
Using optimized parameters selected after seeing OOS.
Using indicators calculated across train+test together incorrectly.
Using market regime labels created with future returns.
```

### Specific trading bugs

#### Bug 1: Indicator warmup leaks test data

Wrong:

```text
calculate indicators on full dataset
then split train/test
```

Usually this is not always fatal for trailing-only indicators, but it can leak if the indicator uses centered windows, future labels, normalization, or full-sample statistics.

Better:

```text
For each fold:
  create train slice
  create test slice with only past warmup before test
  calculate features using only data available up to each bar
```

#### Bug 2: Trades opened in train close in test

If you optimize based on training trades that close inside the test period, you have leakage.

Fix:

```text
purge training trades whose outcome overlaps the test period
```

#### Bug 3: Signal formed before test, trade entered in test

You need a rule.

Either:

```text
Test starts flat: no pre-existing setup allowed.
```

or:

```text
Allow live-style state warmup, but make sure all state comes only from pre-test historical data.
```

The second option is more realistic, but more complex.

#### Bug 4: One global parameter search

Wrong:

```text
Optimize once on all 2 years.
Then run WFO.
```

Correct:

```text
Each fold must optimize only on that fold's training data.
```

#### Bug 5: OOS used repeatedly

If you keep changing the strategy after looking at WFO OOS results, the OOS becomes contaminated.

Solution:

```text
Use WFO OOS for research diagnostics.
Keep a final untouched holdout period.
Only test final selected strategy once on the final holdout.
```

---

## 11. Embargo / gap between train and test

You are correct that a gap can be needed.

But the gap should not be arbitrary. It should depend on:

```text
max indicator lookback
max signal confirmation horizon
max trade holding period
whether labels/trade outcomes overlap
whether state can carry across boundary
```

For H4 strategies:

```text
Small gap: 1–3 bars
Medium gap: 1–3 days
Large gap: max holding period
```

If you have trades that can last 20 H4 bars, and training labels include trade outcome, you may need to purge/embargo around the boundary.

For your case, the gap between train and test appears to be around 4 days. That may be okay, but the bigger issue is not the gap. The bigger issue is **too little train/test data**.

---

## 12. Why WFO is not enough

WFO answers only one question:

> “Did the optimize-then-trade process work on these historical windows?”

It does not fully answer:

```text
Was the result just lucky?
Did I test too many variants?
Is the Sharpe inflated?
Will drawdown be worse live?
Is the edge dependent on one market regime?
Is the edge concentrated in one pair?
Is the parameter choice stable?
Would costs/slippage destroy it?
Is the strategy robust to missed trades, latency, partial fills?
```

That is why quants also use Monte Carlo, DSR, multiple-testing correction, stress tests, parameter plateaus, and regime analysis.

---

## 13. Monte Carlo: why you need it

Monte Carlo tests path risk.

Even if your OOS trades are profitable, the order of trades may be lucky.

Example:

```text
Actual OOS:
win, win, win, loss, win, loss

Alternative possible path:
loss, loss, loss, win, win, win
```

Same trades, different emotional/capital path.

Monte Carlo can test:

```text
probability of ruin
expected max drawdown
95% worst-case drawdown
probability of losing money after N trades
probability of hitting -20% drawdown
confidence interval of expectancy
```

For your system, I would do Monte Carlo on:

```text
combined OOS trades
per-strategy trades
portfolio trades
per-regime trades
per-pair trades
```

Useful pass criteria:

```text
MC 5th percentile expectancy > 0
MC ruin probability < 1–5%
MC 95th percentile drawdown acceptable
MC lower-bound CAGR/PnL still acceptable
```

Monte Carlo is especially important because crypto strategies often have clustered losses.

---

## 14. Romano–Wolf: what it is and why it matters

You wrote “Roman Wolf”; the usual term is **Romano–Wolf**.

Romano–Wolf is a multiple-hypothesis correction method. It helps when you test many strategies, parameters, pairs, or variants.

If you test:

```text
20 strategies
10 parameter sets each
30 pairs
3 timeframes
2 exit modes
```

You are not testing one idea.

You are testing:

```text
20 × 10 × 30 × 3 × 2 = 36,000 variants
```

Some will look good by luck.

Romano–Wolf controls the probability of falsely declaring at least one bad strategy as significant among many tested hypotheses. It is generally more powerful than simple Bonferroni/Holm corrections because it uses resampling and accounts for dependence across tests. ([econ.uzh.ch][1])

In your project, this matters a lot because you test many:

```text
pairs
timeframes
IFVG variants
risk/reward variants
exit modes
filters
regime conditions
```

Without multiple-testing correction, your “best” strategy may simply be the luckiest survivor.

---

## 15. DSR / PSR: why quants use them

### PSR: Probabilistic Sharpe Ratio

PSR asks:

> “Given my sample size, skewness, and kurtosis, what is the probability that the true Sharpe is above a benchmark?”

This is useful because trading returns are not normally distributed.

They often have:

```text
fat tails
skew
serial correlation
volatility clustering
rare large losses
```

Bailey and López de Prado’s PSR adjusts the interpretation of Sharpe for sample length and non-normal return distributions. ([David H Bailey][2])

### DSR: Deflated Sharpe Ratio

DSR goes further.

It asks:

> “After accounting for multiple testing and non-normal returns, is this Sharpe still impressive?”

DSR corrects for two major causes of inflated backtest performance: multiple testing / selection bias and non-normal returns. ([SSRN][3])

For your strategy lab, DSR is very relevant because you are not testing one strategy. You are testing a large search space.

Use DSR when comparing:

```text
many strategy variants
many parameter combinations
many pairs
many timeframes
many exits
many portfolio combinations
```

---

## 16. White’s Reality Check and Hansen SPA

White’s Reality Check is designed for data snooping: when the same dataset is reused many times to find profitable strategies. White explicitly frames the problem as satisfactory historical results possibly being due to chance after repeated reuse of the same data. ([Social Science Computing Core][4])

Hansen’s SPA test is a related improvement used to test whether the best model has superior predictive ability after accounting for data snooping. Research on technical trading rules often uses White’s Reality Check and Hansen’s SPA to correct for data-snooping bias. ([IDEAS/RePEc][5])

For you:

```text
WFO = process validation
Reality Check / SPA = “is the best strategy truly better after data snooping?”
DSR = “is the selected Sharpe still valid after multiple trials?”
Romano–Wolf = “which strategies survive multiple-hypothesis correction?”
```

---

## 17. Purged / embargoed CV and CPCV

López de Prado popularized purged cross-validation and embargo techniques for financial ML/time-series validation.

The idea:

```text
Remove training samples whose information overlaps the test samples.
Add an embargo around test periods to reduce leakage from overlapping labels/events.
```

This matters when labels are path-dependent, for example:

```text
triple-barrier labels
future-return labels
trade outcomes
max holding period labels
meta-labeling
regime labels based on future returns
```

For rule-based strategies, you do not always need full CPCV, but the same concept applies:

```text
Do not let training trades use future test outcomes.
Do not let signals depend on future bars.
Do not let event horizons overlap incorrectly.
```

---

## 18. Parameter plateau analysis

This is one of the most important practical robustness techniques.

Bad optimization result:

```text
Only one exact parameter combination works.
Neighboring values fail.
```

Good result:

```text
A broad area of nearby parameters works.
```

Example:

```text
Good:
ATR multiplier 1.8, 2.0, 2.2, 2.4 all profitable.

Bad:
ATR multiplier 2.13 works,
2.12 fails,
2.14 fails.
```

For every optimized strategy, inspect:

```text
heatmaps
neighbor stability
rank stability
top-N parameter clusters
median performance around selected params
```

Your deployment/research PF ratio idea is also related. If performance is extremely dependent on one exit overlay or one parameter pocket, treat it as fragile.

---

## 19. Regime robustness

A strategy can pass WFO but only because the OOS windows were friendly.

You should split results by regime:

```text
bull market
bear market
sideways market
high volatility
low volatility
high funding / low funding
BTC risk-on
BTC risk-off
trend regime
chop regime
news/event shock regime
```

For each regime:

```text
trades
PF
expectancy
drawdown
win rate
average R
max consecutive losses
```

A strong strategy does not need to win in every regime, but you need to know where it should be enabled or disabled.

For your IFVG strategies, regime analysis is probably as important as WFO.

---

## 20. Cost/slippage/latency stress tests

A strategy that passes WFO with ideal execution may fail live.

You should rerun the strategy with:

```text
base fees
2x fees
3x fees
base slippage
2x slippage
spread widening
missed entries
partial fills
delayed entry by 1 bar
delayed exit by 1 bar
worse stop fill
worse TP fill
funding costs
borrow/financing costs if applicable
```

For crypto micro accounts, this is critical.

A strategy is much stronger if it survives:

```text
+1 tick worse entry
+1 tick worse exit
2x fee
2x slippage
```

For H4, this should be survivable. If H4 strategy dies from tiny cost changes, the edge is probably weak.

---

## 21. Trade resampling / bootstrap

This is different from basic Monte Carlo.

You can bootstrap trades to estimate confidence intervals:

```text
expectancy CI
PF CI
Sharpe CI
drawdown CI
win-rate CI
average R CI
```

Important:

Do not only use IID bootstrap if trades are clustered.

Use:

```text
simple bootstrap
block bootstrap
stationary bootstrap
regime-aware bootstrap
pair-aware bootstrap
```

Why?

Because trading losses are not independent. Bad trades cluster in bad regimes.

---

## 22. Final untouched holdout

This is very important.

Recommended research structure:

```text
1. Development data
   Used for strategy design.

2. WFO / validation data
   Used for model selection and robustness checks.

3. Final holdout
   Used once at the end.

4. Paper trading
   Real-time validation.

5. Small live capital
   Execution validation.
```

If you keep looking at WFO and modifying the strategy, WFO becomes part of development.

So you still need final holdout.

---

## 23. Common WFO bugs checklist

Here is the practical bug list I would use in your codebase.

### Data bugs

```text
[ ] Duplicate candles
[ ] Missing candles
[ ] Wrong timezone
[ ] DST issues
[ ] Exchange maintenance gaps
[ ] Wrong H4 candle boundaries
[ ] Using local time instead of UTC
[ ] Bad OHLC aggregation from ticks/M1
[ ] Survivorship bias in symbol universe
```

### Feature bugs

```text
[ ] Indicator uses future bars
[ ] Centered rolling windows
[ ] Full-sample normalization
[ ] Future high/low used before candle close
[ ] Daily high/low used intraday before day ends
[ ] Regime labels use future returns
```

### Split bugs

```text
[ ] Test data included in optimization
[ ] Embargo missing when labels overlap
[ ] Trades cross train/test boundary incorrectly
[ ] Warmup uses future data
[ ] Fold dates off by one bar
[ ] Test windows overlap but are treated as independent
```

### Optimization bugs

```text
[ ] Optimizing directly on OOS
[ ] Selecting by PF only with tiny trade count
[ ] Ignoring drawdown
[ ] Ignoring parameter stability
[ ] Too many parameters for too few trades
[ ] Reusing failed OOS to redesign strategy endlessly
```

### Execution bugs

```text
[ ] Backtest assumes impossible fills
[ ] SL and TP same-candle ambiguity handled optimistically
[ ] No spread
[ ] No slippage
[ ] No fees
[ ] No funding
[ ] No partial fills
[ ] No min notional / step size / tick size
[ ] No latency
```

---

## 24. What an ideal robustness stack looks like

For your algo trading research, I would use this hierarchy:

```text
Stage 1: Basic backtest
- Long history
- Realistic fees/slippage
- No obvious leakage

Stage 2: Parameter plateau
- Heatmaps
- Neighbor stability
- Avoid sharp single-point optimum

Stage 3: WFO
- Train only on past
- Test on unseen future
- Non-overlapping OOS tests
- Combined OOS equity curve

Stage 4: Final holdout
- Untouched until the end
- Used once

Stage 5: Monte Carlo / bootstrap
- Trade resampling
- Block bootstrap
- Ruin probability
- Drawdown distribution

Stage 6: Multiple testing correction
- DSR / PSR
- Romano–Wolf
- White Reality Check / Hansen SPA if many variants

Stage 7: Regime analysis
- Bull / bear / chop / high vol / low vol
- Per-pair and per-timeframe attribution

Stage 8: Execution stress
- Fees
- Slippage
- latency
- missed fills
- same-candle ambiguity
- funding

Stage 9: Paper trading
- Backtest/live parity
- Signal parity
- Fill realism
- Latency and exchange constraints

Stage 10: Tiny live
- Small capital
- Real fees/fills
- Drift monitoring
```

---

## 25. Recommended WFO rules for your H4 strategies

For your current H4 setup, I would use these rules:

```text
Do not require 5 OOS trades per fold as a hard pass/fail.
Require enough total combined OOS trades instead.
```

Suggested thresholds:

```text
Per fold:
- OOS trades >= 3 preferred
- OOS trades < 3 means fold is low-information, not automatic strategy death

Combined OOS:
- >= 30 trades: minimum
- >= 50 trades: decent
- >= 100 trades: strong

IS:
- >= 30 trades: minimum
- >= 50 trades: better
- >= 100 trades: strong
```

For folds:

```text
2 years H4:
- Use 3–4 folds, not 5 tiny folds
- Or use 5 folds with overlapping training windows

4–6 years H4:
- Use 5–8 folds

10 years FX / crypto:
- Use 6–12 folds depending on trade frequency
```

For train/test duration:

```text
H4 low-frequency:
- Train: 9–24 months
- Test: 2–6 months

H1:
- Train: 6–18 months
- Test: 1–3 months

M15:
- Train: 3–12 months
- Test: 2–8 weeks
```

---

## 26. My direct verdict on your current WFO

Your current setup is **too trade-starved**.

The main issues:

```text
1. Training windows are too short: 613 H4 bars ≈ 102 days.
2. Test windows are too short: 239 H4 bars ≈ 40 days.
3. 5 folds on only 2 years makes each fold too small.
4. Per-fold OOS trade threshold is too strict for low-frequency H4.
5. IS >= 20 trades is too weak for optimization.
6. You are likely rejecting strategies because of bad validation geometry, not because the alpha is dead.
7. Your fold design wastes data because training windows do not need to be disjoint.
```

Best fix:

```text
Use overlapping rolling/anchored training windows.
Keep test windows non-overlapping.
Use fewer folds or longer history.
Judge combined OOS trade count, not only per-fold count.
Add final holdout, Monte Carlo, DSR, multiple-testing correction, regime tests, and execution stress.
```

The most important mental model:

> WFO is not proof. WFO is only one filter. A strategy is much more credible when it passes WFO, has enough OOS trades, has stable parameters, survives Monte Carlo, survives costs, survives multiple-testing correction, and behaves reasonably across regimes.

[1]: https://www.econ.uzh.ch/dam/jcr%3Affffffff-935a-b0d6-ffff-ffffd823d949/jasa.pdf?utm_source=chatgpt.com "Exact and Approximate Stepdown Methods for Multiple ..."
[2]: https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf?utm_source=chatgpt.com "THE DEFLATED SHARPE RATIO"
[3]: https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551&utm_source=chatgpt.com "The Deflated Sharpe Ratio: Correcting for Selection Bias ..."
[4]: https://www.ssc.wisc.edu/~bhansen/718/White2000.pdf?utm_source=chatgpt.com "A REALITY CHECK FOR DATA SNOOPING ..."
[5]: https://ideas.repec.org/a/oup/jfinec/v3y2005i4p606-628.html?utm_source=chatgpt.com "Reexamining the Profitability of Technical Analysis with ..."
