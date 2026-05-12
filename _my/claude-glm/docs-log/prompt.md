Runs super fast!

Let's make sure we implemented all strategies with all grid params as we had in this old Python strategies (especially gird params):
tmp/quant_scout-7/strategies/bollinger_breakout.py
tmp/quant_scout-7/strategies/donchian.py
tmp/quant_scout-7/strategies/dual_momentum.py
tmp/quant_scout-7/strategies/ema_crossover.py
tmp/quant_scout-7/strategies/keltner_breakout.py
tmp/quant_scout-7/strategies/keltner_squeeze.py
tmp/quant_scout-7/strategies/macd.py
tmp/quant_scout-7/strategies/rsi_bb_mr.py
tmp/quant_scout-7/strategies/rsi2.py
tmp/quant_scout-7/strategies/supertrend.py
tmp/quant_scout-7/strategies/trend_pullback.py
tmp/quant_scout-7/strategies/tsmom.py
tmp/quant_scout-7/strategies/vol_compression_breakout.py

Let's also try on top 40 coins from Binance Futures:
AAVEUSDTUSDT
ADAUSDTUSDT
APTUSDTUSDT
ARBUSDTUSDT
AVAXUSDTUSDT
BCHUSDTUSDT
BNBUSDTUSDT
BTCUSDTUSDT
CRVUSDTUSDT
DOGEUSDTUSDT
DOTUSDTUSDT
ENAUSDTUSDT
ETHUSDTUSDT
FETUSDTUSDT
FILUSDTUSDT
HBARUSDTUSDT
HYPEUSDTUSDT
INJUSDTUSDT
LDOUSDTUSDT
LINKUSDTUSDT
LTCUSDTUSDT
NEARUSDTUSDT
OPUSDTUSDT
ORDIUSDTUSDT
PENGUUSDTUSDT
RUNEUSDTUSDT
SOLUSDTUSDT
SUIUSDTUSDT
TAOUSDTUSDT
TIAUSDTUSDT
TONUSDTUSDT
TRXUSDTUSDT
UNIUSDTUSDT
VIRTUALUSDTUSDT
WIFUSDTUSDT
WLDUSDTUSDT
XLMUSDTUSDT
XMRUSDTUSDT
XRPUSDTUSDT
ZECUSDTUSDT


What about parameters plateaus findings and walk forward:
vendor/flox/docs/how-to/walk-forward.md
vendor/flox/docs/how-to/whites-reality-check.md
Think on how can we find not just Sharpe spikes, but robust edges!

In my old Python project we were doing something like this:
tmp/quant_scout-7/pipeline/robustness.py
tmp/quant_scout-7/pipeline/prove.py
tmp/quant_scout-7/pipeline/wfo.py
tmp/quant_scout-7/pipeline/trade_diagnostics.py
tmp/quant_scout-7/pipeline/surface.py 
what does make sense to port into our C++ current project?