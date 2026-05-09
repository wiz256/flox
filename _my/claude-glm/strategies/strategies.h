#pragma once
// Common includes for all strategies
#include "flox/strategy/strategy.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/common.h"

#include <cmath>
#include <deque>
#include <vector>

namespace flox::strategy
{

// ---------------------------------------------------------------------------
// Helper: simple streaming SMA
// ---------------------------------------------------------------------------
inline double streamingSma(const std::deque<double>& buf, size_t period)
{
    if (buf.size() < period) return std::nan("");
    double sum = 0.0;
    auto it = buf.end();
    for (size_t i = 0; i < period; ++i) sum += *--it;
    return sum / static_cast<double>(period);
}

// ---------------------------------------------------------------------------
// Helper: simple streaming EMA
// ---------------------------------------------------------------------------
class StreamingEma
{
public:
    explicit StreamingEma(size_t period) : _period(period), _mult(2.0 / (static_cast<double>(period) + 1.0)) {}

    void update(double v)
    {
        if (!_seeded)
        {
            _ema = v;
            _count = 1;
            _seeded = true;
            return;
        }
        _ema = (v - _ema) * _mult + _ema;
        ++_count;
    }

    double value() const { return _seeded ? _ema : std::nan(""); }
    bool ready() const { return _count >= _period; }
    void reset() { _ema = 0.0; _count = 0; _seeded = false; }

private:
    size_t _period;
    double _mult;
    double _ema = 0.0;
    size_t _count = 0;
    bool _seeded = false;
};

// ---------------------------------------------------------------------------
// Helper: streaming ATR (via RMA)
// ---------------------------------------------------------------------------
class StreamingAtr
{
public:
    explicit StreamingAtr(size_t period) : _period(period), _mult(1.0 / static_cast<double>(period)) {}

    void update(double high, double low, double close)
    {
        double tr = std::abs(high - low);
        if (_seeded)
        {
            tr = std::max({tr, std::abs(high - _prevClose), std::abs(low - _prevClose)});
        }
        if (!_seeded)
        {
            _atr = tr;
            _count = 1;
            _seeded = true;
        }
        else
        {
            _atr = (_atr * static_cast<double>(_count) + tr) * _mult;
            // Equivalent to: _atr = _atr * (1 - _mult) + tr * _mult after first _period bars
            // Simpler: use RMA formula
            _atr = (static_cast<double>(_count - 1) * _atr + tr) / static_cast<double>(_count);
            if (_count < _period * 2) ++_count;
        }
        _prevClose = close;
    }

    double value() const { return _count >= _period ? _atr : std::nan(""); }
    bool ready() const { return _count >= _period; }
    void reset() { _atr = 0.0; _count = 0; _seeded = false; _prevClose = 0.0; }

private:
    size_t _period;
    double _mult;
    double _atr = 0.0;
    double _prevClose = 0.0;
    size_t _count = 0;
    bool _seeded = false;
};

// ---------------------------------------------------------------------------
// Helper: streaming RSI
// ---------------------------------------------------------------------------
class StreamingRsi
{
public:
    explicit StreamingRsi(size_t period) : _period(period), _gainEma(period), _lossEma(period) {}

    void update(double close)
    {
        if (!_seeded)
        {
            _prevClose = close;
            _seeded = true;
            return;
        }
        double diff = close - _prevClose;
        _gainEma.update(diff > 0 ? diff : 0.0);
        _lossEma.update(diff < 0 ? -diff : 0.0);
        _prevClose = close;
        ++_count;
    }

    double value() const
    {
        if (_count < _period) return std::nan("");
        double gain = _gainEma.value();
        double loss = _lossEma.value();
        if (loss < 1e-10) return 100.0;
        double rs = gain / loss;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    bool ready() const { return _count >= _period; }
    void reset() { _gainEma.reset(); _lossEma.reset(); _prevClose = 0.0; _seeded = false; _count = 0; }

private:
    size_t _period;
    StreamingEma _gainEma;
    StreamingEma _lossEma;
    double _prevClose = 0.0;
    size_t _count = 0;
    bool _seeded = false;
};

// ===========================================================================
// 1. DONCHIAN BREAKOUT
// ===========================================================================
struct DonchianBreakoutParams
{
    int period = 20;
    double atrMultSl = 2.0;  // stop-loss ATR multiplier
    std::string toString() const
    {
        return "period=" + std::to_string(period) + ",atr_sl=" + std::to_string(atrMultSl);
    }
};

class DonchianBreakoutStrategy : public Strategy
{
public:
    DonchianBreakoutStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                             DonchianBreakoutParams params = {})
        : Strategy(id, sym, reg), _params(params), _atr(params.period) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble();
        double l = ev.bar.low.toDouble();
        double c = ev.bar.close.toDouble();

        _highs.push_back(h);
        _lows.push_back(l);
        if (_highs.size() > static_cast<size_t>(_params.period))
        {
            _highs.pop_front();
            _lows.pop_front();
        }
        _atr.update(h, l, c);

        if (_highs.size() < static_cast<size_t>(_params.period)) return;
        if (!_atr.ready()) return;

        double upper = *std::max_element(_highs.begin(), _highs.end());
        double lower = *std::min_element(_lows.begin(), _lows.end());
        double atrVal = _atr.value();

        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0)
        {
            if (c > upper)
            {
                emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
                _stopLoss = c - _params.atrMultSl * atrVal;
            }
            else if (c < lower)
            {
                emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
                _stopLoss = c + _params.atrMultSl * atrVal;
            }
        }
        else
        {
            // Check stop-loss
            if (pos.raw() > 0 && c < _stopLoss) emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && c > _stopLoss) emitClosePosition(ev.symbol);
        }
    }

private:
    DonchianBreakoutParams _params;
    std::deque<double> _highs, _lows;
    StreamingAtr _atr;
    double _stopLoss = 0.0;
};

// ===========================================================================
// 2. DUAL MOMENTUM
// ===========================================================================
struct DualMomentumParams
{
    int lookback = 12;      // bars
    double smaPeriod = 20;
    std::string toString() const
    {
        return "lookback=" + std::to_string(lookback) + ",sma=" + std::to_string(smaPeriod);
    }
};

class DualMomentumStrategy : public Strategy
{
public:
    DualMomentumStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                         DualMomentumParams params = {})
        : Strategy(id, sym, reg), _params(params) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_params.lookback + 1))
            _closes.pop_front();

        if (_closes.size() < static_cast<size_t>(_params.lookback + 1)) return;

        // Absolute momentum: current vs N bars ago
        double momentum = _closes.back() - _closes.front();

        // Trend filter: price above SMA
        double sma = streamingSma(_closes, static_cast<size_t>(_params.smaPeriod));
        if (std::isnan(sma)) return;

        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0)
        {
            if (momentum > 0 && c > sma)
                emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (momentum < 0 && c < sma)
                emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        else
        {
            // Exit on momentum reversal or cross below SMA
            if (pos.raw() > 0 && (momentum < 0 || c < sma))
                emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && (momentum > 0 || c > sma))
                emitClosePosition(ev.symbol);
        }
    }

private:
    DualMomentumParams _params;
    std::deque<double> _closes;
};

// ===========================================================================
// 3. EMA CROSSOVER
// ===========================================================================
struct EmaCrossoverParams
{
    int fastPeriod = 12;
    int slowPeriod = 26;
    double atrMultSl = 2.0;
    std::string toString() const
    {
        return "fast=" + std::to_string(fastPeriod) + ",slow=" + std::to_string(slowPeriod)
               + ",atr_sl=" + std::to_string(atrMultSl);
    }
};

class EmaCrossoverStrategy : public Strategy
{
public:
    EmaCrossoverStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                         EmaCrossoverParams params = {})
        : Strategy(id, sym, reg), _params(params),
          _fastEma(static_cast<size_t>(params.fastPeriod)),
          _slowEma(static_cast<size_t>(params.slowPeriod)),
          _atr(static_cast<size_t>(params.slowPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble();
        double l = ev.bar.low.toDouble();
        double c = ev.bar.close.toDouble();

        _fastEma.update(c);
        _slowEma.update(c);
        _atr.update(h, l, c);

        if (!_fastEma.ready() || !_slowEma.ready() || !_atr.ready()) return;

        double fast = _fastEma.value();
        double slow = _slowEma.value();
        double atrVal = _atr.value();
        bool golden = fast > slow;
        Quantity pos = position(ev.symbol);

        if (golden && !_prevGolden && pos.raw() <= 0)
        {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            _stopLoss = c - _params.atrMultSl * atrVal;
        }
        else if (!golden && _prevGolden && pos.raw() >= 0)
        {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
            _stopLoss = c + _params.atrMultSl * atrVal;
        }

        // Stop-loss check
        if (pos.raw() > 0 && c < _stopLoss) emitClosePosition(ev.symbol);
        else if (pos.raw() < 0 && c > _stopLoss) emitClosePosition(ev.symbol);

        _prevGolden = golden;
    }

private:
    EmaCrossoverParams _params;
    StreamingEma _fastEma, _slowEma;
    StreamingAtr _atr;
    bool _prevGolden = false;
    double _stopLoss = 0.0;
};

// ===========================================================================
// 4. KELTNER BREAKOUT
// ===========================================================================
struct KeltnerBreakoutParams
{
    int emaPeriod = 20;
    int atrPeriod = 10;
    double atrMult = 2.0;
    double atrMultSl = 2.5;
    std::string toString() const
    {
        return "ema=" + std::to_string(emaPeriod) + ",atr=" + std::to_string(atrPeriod)
               + ",mult=" + std::to_string(atrMult) + ",sl=" + std::to_string(atrMultSl);
    }
};

class KeltnerBreakoutStrategy : public Strategy
{
public:
    KeltnerBreakoutStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                            KeltnerBreakoutParams params = {})
        : Strategy(id, sym, reg), _params(params),
          _ema(static_cast<size_t>(params.emaPeriod)),
          _atr(static_cast<size_t>(params.atrPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble();
        double l = ev.bar.low.toDouble();
        double c = ev.bar.close.toDouble();

        _ema.update(c);
        _atr.update(h, l, c);

        if (!_ema.ready() || !_atr.ready()) return;

        double mid = _ema.value();
        double atrVal = _atr.value();
        double upper = mid + _params.atrMult * atrVal;
        double lower = mid - _params.atrMult * atrVal;
        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0)
        {
            if (c > upper)
            {
                emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
                _stopLoss = mid - _params.atrMultSl * atrVal;
            }
            else if (c < lower)
            {
                emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
                _stopLoss = mid + _params.atrMultSl * atrVal;
            }
        }
        else
        {
            // Mean-revert exit or stop
            if (pos.raw() > 0 && (c < lower || c < _stopLoss))
                emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && (c > upper || c > _stopLoss))
                emitClosePosition(ev.symbol);
        }
    }

private:
    KeltnerBreakoutParams _params;
    StreamingEma _ema;
    StreamingAtr _atr;
    double _stopLoss = 0.0;
};

// ===========================================================================
// 5. KELTNER SQUEEZE
// ===========================================================================
struct KeltnerSqueezeParams
{
    int bbPeriod = 20;
    double bbStd = 2.0;
    int keltnerEma = 20;
    int keltnerAtr = 10;
    double keltnerMult = 1.5;
    std::string toString() const
    {
        return "bb=" + std::to_string(bbPeriod) + ",kema=" + std::to_string(keltnerEma)
               + ",katr=" + std::to_string(keltnerAtr);
    }
};

class KeltnerSqueezeStrategy : public Strategy
{
public:
    KeltnerSqueezeStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                           KeltnerSqueezeParams params = {})
        : Strategy(id, sym, reg), _params(params),
          _bbSma(static_cast<size_t>(params.bbPeriod)),
          _kema(static_cast<size_t>(params.keltnerEma)),
          _katr(static_cast<size_t>(params.keltnerAtr)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble();
        double l = ev.bar.low.toDouble();
        double c = ev.bar.close.toDouble();

        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_params.bbPeriod))
            _closes.pop_front();

        _bbSma.update(c);
        _kema.update(c);
        _katr.update(h, l, c);

        if (!_bbSma.ready() || !_kema.ready() || !_katr.ready()) return;
        if (_closes.size() < static_cast<size_t>(_params.bbPeriod)) return;

        // BB width
        double mean = _bbSma.value();
        double sumSq = 0.0;
        for (double p : _closes)
        {
            double d = p - mean;
            sumSq += d * d;
        }
        double stddev = std::sqrt(sumSq / static_cast<double>(_closes.size()));
        double bbUpper = mean + _params.bbStd * stddev;
        double bbLower = mean - _params.bbStd * stddev;

        // Keltner
        double kMid = _kema.value();
        double atrVal = _katr.value();
        double kUpper = kMid + _params.keltnerMult * atrVal;
        double kLower = kMid - _params.keltnerMult * atrVal;

        // Squeeze: BB inside Keltner
        bool squeezing = (bbLower > kLower) && (bbUpper < kUpper);
        Quantity pos = position(ev.symbol);

        // Enter on squeeze release + breakout
        if (!_wasSqueezing && _prevSqueezing && pos.raw() == 0)
        {
            if (c > bbUpper)
                emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (c < bbLower)
                emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        // Exit on re-entry to squeeze or BB cross
        else if (pos.raw() > 0 && (c < mean || squeezing))
            emitClosePosition(ev.symbol);
        else if (pos.raw() < 0 && (c > mean || squeezing))
            emitClosePosition(ev.symbol);

        _prevSqueezing = _wasSqueezing;
        _wasSqueezing = squeezing;
    }

private:
    KeltnerSqueezeParams _params;
    std::deque<double> _closes;
    StreamingEma _bbSma, _kema;
    StreamingAtr _katr;
    bool _wasSqueezing = false;
    bool _prevSqueezing = false;
};

// ===========================================================================
// 6. SUPERTREND
// ===========================================================================
struct SupertrendParams
{
    int atrPeriod = 10;
    double atrMult = 3.0;
    std::string toString() const
    {
        return "atr=" + std::to_string(atrPeriod) + ",mult=" + std::to_string(atrMult);
    }
};

class SupertrendStrategy : public Strategy
{
public:
    SupertrendStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                       SupertrendParams params = {})
        : Strategy(id, sym, reg), _params(params),
          _atr(static_cast<size_t>(params.atrPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble();
        double l = ev.bar.low.toDouble();
        double c = ev.bar.close.toDouble();

        _atr.update(h, l, c);
        if (!_atr.ready()) return;

        double atrVal = _atr.value();
        double hl2 = (h + l) / 2.0;
        double rawUpper = hl2 + _params.atrMult * atrVal;
        double rawLower = hl2 - _params.atrMult * atrVal;

        // Supertrend band logic
        if (std::isnan(_upperBand) || rawUpper < _upperBand || _prevClose > _upperBand)
            _upperBand = rawUpper;
        if (std::isnan(_lowerBand) || rawLower > _lowerBand || _prevClose < _lowerBand)
            _lowerBand = rawLower;

        int dir;
        if (_prevDir == -1)
            dir = (c > _upperBand) ? 1 : -1;
        else
            dir = (c < _lowerBand) ? -1 : (_prevDir == 0 ? 1 : _prevDir);

        Quantity pos = position(ev.symbol);

        // Entry on direction change
        if (dir == 1 && _prevDir == -1 && pos.raw() <= 0)
        {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        }
        else if (dir == -1 && _prevDir == 1 && pos.raw() >= 0)
        {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }

        _prevClose = c;
        _prevDir = dir;
    }

private:
    SupertrendParams _params;
    StreamingAtr _atr;
    double _upperBand = std::nan("");
    double _lowerBand = std::nan("");
    double _prevClose = 0.0;
    int _prevDir = 0;
};

// ===========================================================================
// 7. TIME-SERIES MOMENTUM (TSMOM)
// ===========================================================================
struct TsmomParams
{
    int lookback = 12;
    double volLookback = 20;
    double targetVol = 0.15;  // annualized target volatility
    std::string toString() const
    {
        return "lb=" + std::to_string(lookback) + ",vol=" + std::to_string(volLookback);
    }
};

class TsmomStrategy : public Strategy
{
public:
    TsmomStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                  TsmomParams params = {})
        : Strategy(id, sym, reg), _params(params),
          _volEma(static_cast<size_t>(params.volLookback)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_params.lookback + 2))
            _closes.pop_front();

        if (_closes.size() >= 2)
        {
            double ret = (_closes[_closes.size() - 1] / _closes[_closes.size() - 2]) - 1.0;
            _volEma.update(ret * ret);
        }

        if (_closes.size() < static_cast<size_t>(_params.lookback + 1)) return;
        if (!_volEma.ready()) return;

        // Momentum: return over lookback period
        double momentum = (_closes.back() / _closes.front()) - 1.0;

        // Volatility scaling
        double variance = _volEma.value();
        double vol = std::sqrt(variance * 6.0 * 365.0);  // annualize from 4H bars
        double size = (vol > 1e-8) ? _params.targetVol / vol : 1.0;
        size = std::min(std::max(size, 0.1), 2.0);

        Quantity pos = position(ev.symbol);

        if (momentum > 0)
        {
            if (pos.raw() <= 0)
            {
                if (pos.raw() < 0) emitClosePosition(ev.symbol);
                emitMarketBuy(ev.symbol, Quantity::fromDouble(size));
            }
        }
        else
        {
            if (pos.raw() >= 0)
            {
                if (pos.raw() > 0) emitClosePosition(ev.symbol);
                emitMarketSell(ev.symbol, Quantity::fromDouble(size));
            }
        }
    }

private:
    TsmomParams _params;
    std::deque<double> _closes;
    StreamingEma _volEma;
};

// ===========================================================================
// 8. RSI-2 (Larry Connors)
// ===========================================================================
struct Rsi2Params
{
    int rsiPeriod = 2;
    int smaPeriod = 200;
    double oversold = 10.0;
    double overbought = 90.0;
    std::string toString() const
    {
        return "rsi=" + std::to_string(rsiPeriod) + ",sma=" + std::to_string(smaPeriod);
    }
};

class Rsi2Strategy : public Strategy
{
public:
    Rsi2Strategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                 Rsi2Params params = {})
        : Strategy(id, sym, reg), _params(params),
          _rsi(static_cast<size_t>(params.rsiPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_params.smaPeriod))
            _closes.pop_front();

        _rsi.update(c);

        if (_closes.size() < static_cast<size_t>(_params.smaPeriod)) return;
        if (!_rsi.ready()) return;

        double sma = streamingSma(_closes, static_cast<size_t>(_params.smaPeriod));
        double rsi = _rsi.value();

        Quantity pos = position(ev.symbol);

        // Long setup: above SMA + RSI oversold -> buy on pullback
        if (c > sma && rsi < _params.oversold && pos.raw() <= 0)
        {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        }
        // Short setup: below SMA + RSI overbought
        else if (c < sma && rsi > _params.overbought && pos.raw() >= 0)
        {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        // Exit long: RSI > 50
        else if (pos.raw() > 0 && rsi > 50.0)
            emitClosePosition(ev.symbol);
        // Exit short: RSI < 50
        else if (pos.raw() < 0 && rsi < 50.0)
            emitClosePosition(ev.symbol);
    }

private:
    Rsi2Params _params;
    std::deque<double> _closes;
    StreamingRsi _rsi;
};

// ===========================================================================
// 9. RSI + BOLLINGER BAND MEAN REVERSION
// ===========================================================================
struct RsiBbMrParams
{
    int bbPeriod = 20;
    double bbStd = 2.0;
    int rsiPeriod = 14;
    double rsiOversold = 30.0;
    double rsiOverbought = 70.0;
    std::string toString() const
    {
        return "bb=" + std::to_string(bbPeriod) + ",rsi=" + std::to_string(rsiPeriod);
    }
};

class RsiBbMrStrategy : public Strategy
{
public:
    RsiBbMrStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg,
                    RsiBbMrParams params = {})
        : Strategy(id, sym, reg), _params(params),
          _bbSma(static_cast<size_t>(params.bbPeriod)),
          _rsi(static_cast<size_t>(params.rsiPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();

        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_params.bbPeriod))
            _closes.pop_front();

        _bbSma.update(c);
        _rsi.update(c);

        if (_closes.size() < static_cast<size_t>(_params.bbPeriod)) return;
        if (!_bbSma.ready() || !_rsi.ready()) return;

        double mean = _bbSma.value();
        double sumSq = 0.0;
        for (double p : _closes) { double d = p - mean; sumSq += d * d; }
        double stddev = std::sqrt(sumSq / static_cast<double>(_closes.size()));
        double bbLower = mean - _params.bbStd * stddev;
        double bbUpper = mean + _params.bbStd * stddev;
        double rsi = _rsi.value();

        Quantity pos = position(ev.symbol);

        // Long: price below lower BB + RSI oversold
        if (c < bbLower && rsi < _params.rsiOversold && pos.raw() <= 0)
        {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        }
        // Short: price above upper BB + RSI overbought
        else if (c > bbUpper && rsi > _params.rsiOverbought && pos.raw() >= 0)
        {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        // Exit long at mean
        else if (pos.raw() > 0 && c >= mean)
            emitClosePosition(ev.symbol);
        // Exit short at mean
        else if (pos.raw() < 0 && c <= mean)
            emitClosePosition(ev.symbol);
    }

private:
    RsiBbMrParams _params;
    std::deque<double> _closes;
    StreamingEma _bbSma;
    StreamingRsi _rsi;
};

}  // namespace flox::strategy
