# FLOX C++ Strategy Implementations

**Date**: 2026-05-09
**Purpose**: Ready-to-use C++ strategy implementations for grid search backtesting with Binance futures data.

---

## Strategy Architecture

Each strategy follows the FLOX `Strategy` base class pattern:
1. Typed parameters via constructor
2. Streaming indicators as members
3. `onSymbolTrade()` or `onBar()` for signal generation
4. Grid-searchable via `BacktestOptimizer`

---

## 1. Donchian Breakout

```cpp
#pragma once
// donchian_breakout.h

#include "flox/strategy/strategy.h"
#include <deque>
#include <algorithm>

class DonchianBreakout : public flox::Strategy {
public:
    struct Params {
        int channelPeriod;     // Lookback for channel (20, 25, 30, 35, 40)
        int atrPeriod;         // ATR period for stops (14)
        double atrSlMult;      // Stop-loss ATR multiplier (2.0-4.0)
        double atrTpMult;      // Take-profit ATR multiplier (1.0-3.0)
        double size;           // Position size in base units
    };

    DonchianBreakout(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
                     Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();

        // Update high/low history
        _highs.push_back(price);
        _lows.push_back(price);
        if (_highs.size() > _params.channelPeriod) _highs.pop_front();
        if (_lows.size() > _params.channelPeriod) _lows.pop_front();

        // Update ATR (simplified - uses true range approximation)
        updateATR(price);

        // Warmup check
        if (_highs.size() < _params.channelPeriod || !_atrReady) return;

        // Channel levels
        double upper = *std::max_element(_highs.begin(), _highs.end());
        double lower = *std::min_element(_lows.begin(), _lows.end());

        // Previous bar's close (for breakout confirmation)
        double prevClose = _prevClose;
        _prevClose = price;
        if (prevClose == 0) return;

        // Entry: close breaks above upper channel
        if (price > upper && prevClose <= _prevUpper && ctx.isFlat()) {
            flox::Quantity qty = flox::Quantity::fromDouble(_params.size);
            emitMarketBuy(symbol(), qty);
            _entryPrice = price;
            _stopPrice = price - _atrValue * _params.atrSlMult;
            _tpPrice = price + _atrValue * _params.atrTpMult;
        }
        // Short entry: close breaks below lower channel
        else if (price < lower && prevClose >= _prevLower && ctx.isFlat()) {
            flox::Quantity qty = flox::Quantity::fromDouble(_params.size);
            emitMarketSell(symbol(), qty);
            _entryPrice = price;
            _stopPrice = price + _atrValue * _params.atrSlMult;
            _tpPrice = price - _atrValue * _params.atrTpMult;
        }
        // Exit long: hit stop or TP
        else if (ctx.isLong()) {
            if (price <= _stopPrice) emitClosePosition(symbol());
            else if (price >= _tpPrice) emitClosePosition(symbol());
        }
        // Exit short: hit stop or TP
        else if (ctx.isShort()) {
            if (price >= _stopPrice) emitClosePosition(symbol());
            else if (price <= _tpPrice) emitClosePosition(symbol());
        }

        _prevUpper = upper;
        _prevLower = lower;
    }

private:
    void updateATR(double price) {
        if (_prevClose == 0) { _prevClose = price; return; }
        double tr = std::abs(price - _prevClose);  // Simplified true range
        _trSum += tr;
        _trCount++;
        if (_trCount >= _params.atrPeriod) {
            _atrValue = _trSum / _params.atrPeriod;
            _atrReady = true;
        }
    }

    Params _params;
    std::deque<double> _highs, _lows;
    double _prevClose{0}, _prevUpper{0}, _prevLower{0};
    double _entryPrice{0}, _stopPrice{0}, _tpPrice{0};
    double _atrValue{0}, _trSum{0};
    int _trCount{0};
    bool _running{false}, _atrReady{false};
};
```

**Grid search:**
```cpp
struct DonchianParams {
    int channel, atrPeriod;
    double slMult, tpMult, size;
    std::string toString() const {
        return fmt::format("ch={},atr={},sl={:.1f},tp={:.1f}",
                          channel, atrPeriod, slMult, tpMult);
    }
};

struct DonchianGrid {
    std::vector<int> channels = {20, 25, 30, 35, 40};
    std::vector<int> atrPeriods = {10, 14, 21};
    std::vector<double> slMults = {2.0, 2.5, 3.0, 3.5, 4.0};
    std::vector<double> tpMults = {1.0, 1.5, 2.0, 3.0};

    size_t totalCombinations() const {
        return channels.size() * atrPeriods.size() * slMults.size() * tpMults.size();
    }

    DonchianParams operator[](size_t i) const {
        size_t nAtr = atrPeriods.size(), nSl = slMults.size(), nTp = tpMults.size();
        return {
            channels[i / (nAtr * nSl * nTp)],
            atrPeriods[(i / (nSl * nTp)) % nAtr],
            slMults[(i / nTp) % nSl],
            tpMults[i % nTp],
            0.01
        };
    }
};
```

---

## 2. Dual Momentum

```cpp
#pragma once
// dual_momentum.h

#include "flox/strategy/strategy.h"
#include <deque>

class DualMomentum : public flox::Strategy {
public:
    struct Params {
        int lookback;        // Momentum lookback (60, 120, 252 bars)
        int smaPeriod;       // Trend filter SMA (50, 100, 200)
        double size;
    };

    DualMomentum(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
                 Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();
        _prices.push_back(price);
        if (_prices.size() > std::max(_params.lookback, _params.smaPeriod) + 1)
            _prices.pop_front();

        if (_prices.size() < _params.lookback + 1) return;

        // Absolute momentum: current price vs price N bars ago
        double momentum = price - _prices[_prices.size() - _params.lookback - 1];

        // Trend filter: SMA
        if (_prices.size() < _params.smaPeriod) return;
        double sma = computeSMA(_params.smaPeriod);

        // Dual momentum: positive momentum + above SMA → long
        if (momentum > 0 && price > sma && ctx.isFlat()) {
            emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (momentum < 0 && price < sma && ctx.isFlat()) {
            emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if ((ctx.isLong() && momentum < 0) || (ctx.isShort() && momentum > 0)) {
            emitClosePosition(symbol());
        }
    }

private:
    double computeSMA(int period) const {
        double sum = 0;
        auto it = _prices.end();
        for (int i = 0; i < period; ++i) sum += *--it;
        return sum / period;
    }

    Params _params;
    std::deque<double> _prices;
    bool _running{false};
};
```

---

## 3. EMA Crossover

```cpp
#pragma once
// ema_crossover.h

#include "flox/strategy/strategy.h"
#include <deque>

class EMACrossover : public flox::Strategy {
public:
    struct Params {
        int fastPeriod;      // Fast EMA (5, 10, 15, 20)
        int slowPeriod;      // Slow EMA (20, 30, 50, 100)
        double size;
    };

    EMACrossover(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
                 Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();

        // Update EMAs
        _fastEma = updateEMA(_fastEma, price, _params.fastPeriod);
        _slowEma = updateEMA(_slowEma, price, _params.slowPeriod);
        _barCount++;

        // Warmup
        if (_barCount < _params.slowPeriod) return;

        bool above = _fastEma > _slowEma;

        // Crossover entry
        if (above && !_prevAbove && ctx.isFlat()) {
            emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (!above && _prevAbove && ctx.isFlat()) {
            emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        // Exit on reverse cross
        else if (ctx.isLong() && !above && _prevAbove) {
            emitClosePosition(symbol());
        }
        else if (ctx.isShort() && above && !_prevAbove) {
            emitClosePosition(symbol());
        }

        _prevAbove = above;
    }

private:
    double updateEMA(double current, double price, int period) {
        double k = 2.0 / (period + 1);
        if (current == 0) return price;
        return price * k + current * (1 - k);
    }

    Params _params;
    double _fastEma{0}, _slowEma{0};
    bool _running{false}, _prevAbove{false};
    int _barCount{0};
};
```

---

## 4. Keltner Breakout

```cpp
#pragma once
// keltner_breakout.h

#include "flox/strategy/strategy.h"
#include <deque>

class KeltnerBreakout : public flox::Strategy {
public:
    struct Params {
        int emaPeriod;       // EMA period (20)
        int atrPeriod;       // ATR period (10, 14, 21)
        double upperMult;    // Upper band multiplier (1.5, 2.0, 2.5, 3.0)
        double lowerMult;    // Lower band multiplier (1.5, 2.0, 2.5, 3.0)
        double size;
    };

    KeltnerBreakout(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
                    Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();
        _prices.push_back(price);
        if (_prices.size() > _params.atrPeriod + 1) _prices.pop_front();

        // Update EMA
        double k = 2.0 / (_params.emaPeriod + 1);
        _ema = (_ema == 0) ? price : price * k + _ema * (1 - k);
        _emaCount++;

        // Compute ATR
        if (_prices.size() < _params.atrPeriod + 1) return;
        double atr = computeATR();

        if (_emaCount < _params.emaPeriod) return;

        double upper = _ema + atr * _params.upperMult;
        double lower = _ema - atr * _params.lowerMult;

        // Breakout entry
        if (price > upper && ctx.isFlat()) {
            emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (price < lower && ctx.isFlat()) {
            emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        // Exit when price returns to EMA
        else if (ctx.isLong() && price < _ema) {
            emitClosePosition(symbol());
        }
        else if (ctx.isShort() && price > _ema) {
            emitClosePosition(symbol());
        }
    }

private:
    double computeATR() const {
        double sum = 0;
        for (size_t i = 1; i < _prices.size(); ++i) {
            sum += std::abs(_prices[i] - _prices[i-1]);
        }
        return sum / (_prices.size() - 1);
    }

    Params _params;
    std::deque<double> _prices;
    double _ema{0};
    int _emaCount{0};
    bool _running{false};
};
```

---

## 5. Keltner Squeeze

```cpp
#pragma once
// keltner_squeeze.h

#include "flox/strategy/strategy.h"
#include <deque>
#include <algorithm>

class KeltnerSqueeze : public flox::Strategy {
public:
    struct Params {
        int emaPeriod;       // EMA period (20)
        int atrPeriod;       // ATR period (14)
        int bbPeriod;        // Bollinger Band period (20)
        double bbStd;        // Bollinger Band std dev (2.0)
        double keltMult;     // Keltner multiplier (1.5)
        double size;
    };

    KeltnerSqueeze(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
                   Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();
        _prices.push_back(price);
        if (_prices.size() > _params.bbPeriod + 1) _prices.pop_front();

        if (_prices.size() < _params.bbPeriod + 1) return;

        // Bollinger Bands
        double sma = computeSMA(_params.bbPeriod);
        double stddev = computeStdDev(_params.bbPeriod, sma);
        double bbUpper = sma + _params.bbStd * stddev;
        double bbLower = sma - _params.bbStd * stddev;

        // Keltner Channels
        double atr = computeATR();
        double keltUpper = sma + _params.keltMult * atr;
        double keltLower = sma - _params.keltMult * atr;

        // Squeeze detection: BB inside Keltner
        bool squeezing = (bbUpper < keltUpper) && (bbLower > keltLower);

        // Entry after squeeze release
        if (!_wasSqueezing && _prevSqueezing && ctx.isFlat()) {
            // Squeeze just released - enter in breakout direction
            if (price > bbUpper) {
                emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
            } else if (price < bbLower) {
                emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
            }
        }
        // Exit on return to SMA
        else if (ctx.isLong() && price < sma) {
            emitClosePosition(symbol());
        }
        else if (ctx.isShort() && price > sma) {
            emitClosePosition(symbol());
        }

        _prevSqueezing = squeezing;
        _wasSqueezing = _prevSqueezing;
    }

private:
    double computeSMA(int period) const {
        double sum = 0;
        auto it = _prices.end();
        for (int i = 0; i < period; ++i) sum += *--it;
        return sum / period;
    }

    double computeStdDev(int period, double mean) const {
        double sum = 0;
        auto it = _prices.end();
        for (int i = 0; i < period; ++i) { double d = *--it - mean; sum += d * d; }
        return std::sqrt(sum / period);
    }

    double computeATR() const {
        double sum = 0;
        for (size_t i = 1; i < _prices.size(); ++i) {
            sum += std::abs(_prices[i] - _prices[i-1]);
        }
        return sum / (_prices.size() - 1);
    }

    Params _params;
    std::deque<double> _prices;
    bool _running{false}, _prevSqueezing{false}, _wasSqueezing{false};
};
```

---

## 6. Supertrend

```cpp
#pragma once
// supertrend.h

#include "flox/strategy/strategy.h"
#include <deque>
#include <algorithm>

class Supertrend : public flox::Strategy {
public:
    struct Params {
        int atrPeriod;       // ATR period (10, 14, 21)
        double multiplier;   // Supertrend multiplier (2.0, 2.5, 3.0, 3.5, 4.0)
        double size;
    };

    Supertrend(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
               Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();
        _highs.push_back(price); // Simplified: using close as high/low
        _lows.push_back(price);
        if (_highs.size() > _params.atrPeriod) { _highs.pop_front(); _lows.pop_front(); }

        if (_highs.size() < _params.atrPeriod) return;

        // ATR
        double atr = 0;
        for (size_t i = 1; i < _highs.size(); ++i) {
            atr += std::max(_highs[i] - _lows[i], std::abs(_highs[i] - _lows[i-1]));
        }
        atr /= _highs.size();

        // Basic bands
        double hl2 = (price + price) / 2;  // Simplified: no separate high/low
        double upperBand = hl2 + _params.multiplier * atr;
        double lowerBand = hl2 - _params.multiplier * atr;

        // Supertrend logic
        double prevUpper = _supertrend > 0 && _supertrend < _prevClose
            ? std::min(upperBand, _supertrend) : upperBand;
        double prevLower = _supertrend > 0 && _supertrend > _prevClose
            ? std::max(lowerBand, _supertrend) : lowerBand;

        double st;
        if (_supertrend == prevUpper) {
            st = (price > prevUpper) ? prevLower : prevUpper;
        } else {
            st = (price < prevLower) ? prevUpper : prevLower;
        }

        if (_supertrend > 0) {
            // Direction change signals
            if (st < _supertrend && ctx.isFlat()) {
                emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
            }
            else if (st > _supertrend && ctx.isFlat()) {
                emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
            }
            else if (ctx.isLong() && st > _supertrend) {
                emitClosePosition(symbol());
            }
            else if (ctx.isShort() && st < _supertrend) {
                emitClosePosition(symbol());
            }
        }

        _prevClose = price;
        _supertrend = st;
    }

private:
    Params _params;
    std::deque<double> _highs, _lows;
    double _supertrend{0}, _prevClose{0};
    bool _running{false};
};
```

---

## 7. TSMOM (Time Series Momentum)

```cpp
#pragma once
// tsmom.h

#include "flox/strategy/strategy.h"
#include <deque>

class TSMOM : public flox::Strategy {
public:
    struct Params {
        int lookback;        // Lookback period (20, 60, 120, 252)
        int volPeriod;       // Volatility lookback (20, 60)
        double size;
    };

    TSMOM(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
          Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const flox::TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();
        _prices.push_back(price);
        if (_prices.size() > _params.lookback + _params.volPeriod + 1)
            _prices.pop_front();

        if (_prices.size() < _params.lookback + _params.volPeriod) return;

        // Time series momentum: return over lookback
        double pastPrice = _prices[_prices.size() - _params.lookback - 1];
        double momentum = (price - pastPrice) / pastPrice;

        // Volatility scaling
        double vol = computeVol(_params.volPeriod);
        double scaledMomentum = (vol > 0) ? momentum / vol : 0;

        // Position based on sign of scaled momentum
        if (scaledMomentum > 0 && ctx.isFlat()) {
            emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (scaledMomentum < 0 && ctx.isFlat()) {
            emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (ctx.isLong() && scaledMomentum < 0) {
            emitClosePosition(symbol());
        }
        else if (ctx.isShort() && scaledMomentum > 0) {
            emitClosePosition(symbol());
        }
    }

private:
    double computeVol(int period) const {
        double sum = 0, sumSq = 0;
        size_t n = _prices.size();
        for (size_t i = n - period; i < n; ++i) {
            double ret = (_prices[i] - _prices[i-1]) / _prices[i-1];
            sum += ret;
            sumSq += ret * ret;
        }
        double mean = sum / period;
        return std::sqrt(sumSq / period - mean * mean);
    }

    Params _params;
    std::deque<double> _prices;
    bool _running{false};
};
```

---

## 8. RSI2 (Extreme Oversold/Overbought)

```cpp
#pragma once
// rsi2.h

#include "flox/strategy/strategy.h"

class RSI2 : public flox::Strategy {
public:
    struct Params {
        double oversold;     // Oversold threshold (5, 10, 15)
        double overbought;   // Overbought threshold (85, 90, 95)
        int emaTrend;        // Trend EMA period (50, 100, 200)
        double size;
    };

    RSI2(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
         Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();

        // Update RSI(2)
        double delta = price - _prevPrice;
        if (_prevPrice > 0) {
            double gain = (delta > 0) ? delta : 0;
            double loss = (delta < 0) ? -delta : 0;

            _avgGain = (_avgGain < 0) ? gain : (_avgGain * 1 + gain) / 2;  // Wilder smoothing
            _avgLoss = (_avgLoss < 0) ? loss : (_avgLoss * 1 + loss) / 2;
            _barCount++;
        }
        _prevPrice = price;

        // Update trend EMA
        double k = 2.0 / (_params.emaTrend + 1);
        _ema = (_ema == 0) ? price : price * k + _ema * (1 - k);

        if (_barCount < 3) return;

        double rsi = (_avgLoss == 0) ? 100 : 100 - 100 / (1 + _avgGain / _avgLoss);

        // Strategy logic:
        // Long when RSI(2) < oversold AND price > EMA (uptrend snap-back)
        // Short when RSI(2) > overbought AND price < EMA (downtrend snap-back)

        if (rsi < _params.oversold && price > _ema && ctx.isFlat()) {
            emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (rsi > _params.overbought && price < _ema && ctx.isFlat()) {
            emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
        }
        else if (ctx.isLong() && rsi > 70) {  // Exit long when RSI recovers
            emitClosePosition(symbol());
        }
        else if (ctx.isShort() && rsi < 30) {  // Exit short when RSI drops
            emitClosePosition(symbol());
        }
    }

private:
    Params _params;
    double _prevPrice{0}, _ema{0};
    double _avgGain{-1}, _avgLoss{-1};
    int _barCount{0};
    bool _running{false};
};
```

---

## 9. RSI_BB_MR (RSI + Bollinger Band Mean Reversion)

```cpp
#pragma once
// rsi_bb_mr.h

#include "flox/strategy/strategy.h"
#include <cmath>
#include <deque>

class RSIBBMR : public flox::Strategy {
public:
    struct Params {
        int rsiPeriod;       // RSI period (2, 4, 14)
        double oversold;     // RSI oversold (10, 20, 30)
        double overbought;   // RSI overbought (70, 80, 90)
        int bbPeriod;        // Bollinger Band period (20)
        double bbStd;        // BB std dev (2.0, 2.5)
        int atrPeriod;       // ATR period for stops (14)
        double atrSlMult;    // Stop loss ATR multiplier (2.0)
        double size;
    };

    RSIBBMR(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
            Params params)
        : Strategy(1, symbol, registry), _params(params) {}

    void start() override { _running = true; }
    void stop() override { _running = false; }

protected:
    void onSymbolTrade(flox::SymbolContext& ctx, const TradeEvent& ev) override {
        if (!_running) return;

        double price = ev.trade.price.toDouble();
        _prices.push_back(price);
        if (_prices.size() > _params.bbPeriod + 1) _prices.pop_front();

        // Update RSI
        updateRSI(price);

        if (_barCount < _params.bbPeriod + 1) return;

        // Bollinger Bands
        double sma = computeSMA(_params.bbPeriod);
        double stddev = computeStdDev(_params.bbPeriod, sma);
        double bbLower = sma - _params.bbStd * stddev;
        double bbUpper = sma + _params.bbStd * stddev;

        // ATR for stop
        double atr = computeATR();

        double rsi = getRSI();
        if (rsi < 0) return;  // Not enough data

        // DOUBLE CONFIRMATION: RSI oversold AND price at/below lower BB
        if (rsi < _params.oversold && price <= bbLower && ctx.isFlat()) {
            emitMarketBuy(symbol(), flox::Quantity::fromDouble(_params.size));
            _stopPrice = price - atr * _params.atrSlMult;
        }
        // DOUBLE CONFIRMATION: RSI overbought AND price at/above upper BB
        else if (rsi > _params.overbought && price >= bbUpper && ctx.isFlat()) {
            emitMarketSell(symbol(), flox::Quantity::fromDouble(_params.size));
            _stopPrice = price + atr * _params.atrSlMult;
        }
        // Exit long: price returns to SMA or hits stop
        else if (ctx.isLong()) {
            if (price >= sma || price <= _stopPrice) {
                emitClosePosition(symbol());
            }
        }
        // Exit short: price returns to SMA or hits stop
        else if (ctx.isShort()) {
            if (price <= sma || price >= _stopPrice) {
                emitClosePosition(symbol());
            }
        }
    }

private:
    void updateRSI(double price) {
        if (_prevPrice > 0) {
            double delta = price - _prevPrice;
            double gain = (delta > 0) ? delta : 0;
            double loss = (delta < 0) ? -delta : 0;
            _avgGain = (_avgGain < 0) ? gain : (_avgGain * (_params.rsiPeriod - 1) + gain) / _params.rsiPeriod;
            _avgLoss = (_avgLoss < 0) ? loss : (_avgLoss * (_params.rsiPeriod - 1) + loss) / _params.rsiPeriod;
            _barCount++;
        }
        _prevPrice = price;
    }

    double getRSI() const {
        if (_avgGain < 0 || _avgLoss < 0) return -1;
        if (_avgLoss == 0) return 100;
        return 100 - 100 / (1 + _avgGain / _avgLoss);
    }

    double computeSMA(int period) const {
        double sum = 0;
        auto it = _prices.end();
        for (int i = 0; i < period; ++i) sum += *--it;
        return sum / period;
    }

    double computeStdDev(int period, double mean) const {
        double sum = 0;
        auto it = _prices.end();
        for (int i = 0; i < period; ++i) { double d = *--it - mean; sum += d * d; }
        return std::sqrt(sum / period);
    }

    double computeATR() const {
        double sum = 0;
        for (size_t i = 1; i < _prices.size(); ++i)
            sum += std::abs(_prices[i] - _prices[i-1]);
        return sum / (_prices.size() - 1);
    }

    Params _params;
    std::deque<double> _prices;
    double _prevPrice{0}, _avgGain{-1}, _avgLoss{-1};
    double _stopPrice{0};
    int _barCount{0};
    bool _running{false};
};
```

---

## Grid Search Main Program

```cpp
// main_grid_search.cpp
// Run all strategies with grid search

#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"

// Include strategy headers
#include "donchian_breakout.h"
#include "ema_crossover.h"
#include "keltner_breakout.h"
#include "rsi2.h"
#include "rsi_bb_mr.h"

#include <iostream>
#include <memory>

// Generic grid search runner
template <typename StrategyT, typename ParamsT, typename GridT>
void runStrategyGrid(
    const std::string& name,
    const GridT& grid,
    flox::SymbolId symbol,
    const flox::SymbolRegistry& registry,
    const std::string& csvPath,
    std::function<flox::BacktestResult(const ParamsT&)> factory)
{
    std::cout << "\n=== " << name << " Grid Search ===\n";

    flox::BacktestOptimizer<ParamsT, GridT> optimizer;
    optimizer.setParameterGrid(grid);
    optimizer.setBacktestFactory(factory);

    auto results = optimizer.runLocal();
    auto ranked = flox::BacktestOptimizer<ParamsT, GridT>::rankResults(
        results, flox::RankMetric::SharpeRatio);

    std::cout << "Total combos: " << results.size() << "\n";
    std::cout << "Top 5:\n";
    for (size_t i = 0; i < std::min(size_t(5), ranked.size()); ++i) {
        const auto& r = ranked[i];
        std::cout << "  " << (i+1) << ". " << r.parameters.toString()
                  << " Sharpe=" << r.sharpeRatio()
                  << " Return=" << r.totalReturn() << "%"
                  << " Trades=" << r.totalTrades() << "\n";
    }

    flox::BacktestOptimizer<ParamsT, GridT>::exportToCSV(ranked, name + "_results.csv");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <csv_path>\n";
        return 1;
    }

    flox::SymbolRegistry registry;
    flox::SymbolInfo info{
        .exchange = "binance", .symbol = "BTCUSDT",
        .tickSize = flox::Price::fromDouble(0.01)
    };
    auto symId = registry.registerSymbol(info);
    std::string csvPath = argv[1];

    // Run grid searches for each strategy
    // (Each would have its own grid definition and factory function)

    // Example: Donchian Breakout
    // runStrategyGrid<DonchianBreakout, DonchianParams, DonchianGrid>(
    //     "donchian_breakout", DonchianGrid{}, symId, registry, csvPath, factory);

    // ... similarly for other strategies

    return 0;
}
```

---

## Python Equivalent for Quick Testing

```python
# run_all_strategies.py
import flox_py as flox
import pandas as pd
from itertools import product

registry = flox.SymbolRegistry()
btc = registry.add_symbol("binance", "BTCUSDT", tick_size=0.01)

def run_grid(strategy_factory, param_grid, csv_path):
    """Run grid search for any strategy."""
    keys = list(param_grid.keys())
    values = list(param_grid.values())
    results = []

    for combo in product(*values):
        params = dict(zip(keys, combo))
        strat = strategy_factory([btc], **params)
        bt = flox.BacktestRunner(registry, fee_rate=0.0004, initial_capital=10_000)
        bt.set_strategy(strat)
        stats = bt.run_csv(csv_path, "BTCUSDT")
        results.append({**params, **stats})

    return pd.DataFrame(results).sort_values("sharpe", ascending=False)

# Example: Donchian Breakout
class DonchianBreakout(flox.Strategy):
    def __init__(self, symbols, channel=20, size=0.01):
        super().__init__(symbols)
        self.channel = channel
        self.size = size
        self.highs = []
        self.lows = []

    def on_trade(self, ctx, trade):
        price = trade.price
        self.highs.append(price)
        self.lows.append(price)
        if len(self.highs) > self.channel:
            self.highs.pop(0)
            self.lows.pop(0)
        if len(self.highs) < self.channel:
            return

        upper = max(self.highs)
        lower = min(self.lows)

        if price > upper and ctx.is_flat():
            self.market_buy(self.size)
        elif price < lower and ctx.is_flat():
            self.market_sell(self.size)

# Run
results = run_grid(
    DonchianBreakout,
    {"channel": [20, 25, 30, 35, 40], "size": [0.01]},
    "data/btcusdt_1m.csv"
)
print(results.head(10))
results.to_csv("donchian_results.csv", index=False)
```

---

## Downloading Binance Futures Data

```python
# download_binance_data.py
import ccxt
import pandas as pd
import time

def download_futures_ohlcv(symbol="BTC/USDT", timeframe="1m",
                           start_date="2020-01-01", end_date="2025-12-31",
                           output_path="data/btcusdt_1m.csv"):
    """Download Binance USDT-M perpetual futures OHLCV data."""
    exchange = ccxt.binance({
        'options': {'defaultType': 'future'},
        'enableRateLimit': True,
    })

    since = exchange.parse8601(f"{start_date}T00:00:00Z")
    end_ts = exchange.parse8601(f"{end_date}T00:00:00Z")

    all_bars = []
    while since < end_ts:
        bars = exchange.fetch_ohlcv(symbol, timeframe, since=since, limit=1500)
        if not bars:
            break
        all_bars.extend(bars)
        since = bars[-1][0] + 1
        time.sleep(exchange.rateLimit / 1000)
        print(f"Downloaded {len(all_bars)} bars, latest: {bars[-1][0]}")

    df = pd.DataFrame(all_bars, columns=['timestamp', 'open', 'high', 'low', 'close', 'volume'])
    df['timestamp'] = pd.to_datetime(df['timestamp'], unit='ms')
    df.to_csv(output_path, index=False)
    print(f"Saved {len(df)} bars to {output_path}")
    return df

if __name__ == "__main__":
    download_futures_ohlcv()
```
