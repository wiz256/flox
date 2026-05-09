# FLOX Deep Q&A and Strategy Reference

**Date**: 2026-05-09

## Files

- [flox-deep-qa.md](flox-deep-qa.md) — Comprehensive answers to all Q1-Q13 and Events deep-dive
- [flox-strategies.cpp](flox-strategies.cpp) — C++ strategy implementations for all 9 strategies with grid search

## Quick Navigation

| Question | Topic | Key Answer |
|----------|-------|------------|
| Q1 | Fork vs separate repo | Start inside forked FLOX repo |
| Q2 | 1M data support | Yes, via CSV reader or numpy arrays |
| Q3 | CSV/parquet data | CSV direct, parquet via numpy |
| Q4 | Indicator calculation | Batch (compute) + streaming (update), calculated on trades |
| Q5 | Tick data availability | Most exchanges don't provide 5-6 years of tick data |
| Q6 | Data updates | Append to parquet or live recording to .floxlog |
| Q7 | Live warmup | Feed historical bars through strategy before connecting to live |
| Q8 | Oryon + FLOX C++ | Bridge in Python layer; not direct C++-to-Rust |
| Q9 | Backtest data feeding | CSV, numpy arrays, .floxlog replay, or mmap bars |
| Q10 | Live memory/warmup | <50MB for 40 symbols; warmup from historical data |
| Q11 | Trading concepts | Full entity explanation (orderbook, positions, orders, etc.) |
| Q11b | Previous bars access | Manual deque, BarMatrix, or last_n_closed_bars |
| Q12 | Bar close vs intrabar | Combine on_trade (indicators) + on_bar (decisions) |
| Q13 | Complex exits | Chandelier, bracket, trailing stop, time stop, etc. |
| Events | All FLOX events | TradeEvent, BookUpdateEvent, BarEvent, OrderEvent flow |
