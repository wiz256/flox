#pragma once
// 13 strategy implementations matching Python quant_scout-7 grid params exactly.
// All strategies use onSymbolBar() for 4H bar-based decisions.

#include "flox/strategy/strategy.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/common.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <vector>

namespace flox::strategy
{

// =====================================================================
// Streaming indicator helpers
// =====================================================================

class StreamingEma
{
public:
    explicit StreamingEma(size_t period) : _period(period), _mult(2.0 / (static_cast<double>(period) + 1.0)) {}
    void update(double v) { if (!_seeded) { _ema = v; _seeded = true; return; } _ema = (v - _ema) * _mult + _ema; ++_count; }
    double value() const { return _seeded ? _ema : std::nan(""); }
    bool ready() const { return _count >= _period; }
    void reset() { _ema = 0.0; _count = 0; _seeded = false; }
private:
    size_t _period; double _mult; double _ema = 0.0; size_t _count = 0; bool _seeded = false;
};

class StreamingSma
{
public:
    explicit StreamingSma(size_t period) : _period(period) {}
    void update(double v) { _buf.push_back(v); if (_buf.size() > _period) _buf.pop_front(); }
    double value() const { if (_buf.size() < _period) return std::nan(""); double s = 0; for (double v : _buf) s += v; return s / static_cast<double>(_buf.size()); }
    bool ready() const { return _buf.size() >= _period; }
    void reset() { _buf.clear(); }
    const std::deque<double>& buf() const { return _buf; }
private:
    size_t _period; std::deque<double> _buf;
};

class StreamingAtr
{
public:
    explicit StreamingAtr(size_t period) : _period(period), _alpha(1.0 / static_cast<double>(period)) {}
    void update(double high, double low, double close)
    {
        double tr = std::max({high - low, std::abs(high - _prevClose), std::abs(low - _prevClose)});
        if (!_seeded) { _atr = tr; _seeded = true; } else { _atr = _atr * (1.0 - _alpha) + tr * _alpha; }
        _prevClose = close; ++_count;
    }
    double value() const { return _count >= _period ? _atr : std::nan(""); }
    bool ready() const { return _count >= _period; }
    void reset() { _atr = 0.0; _count = 0; _seeded = false; _prevClose = 0.0; }
private:
    size_t _period; double _alpha; double _atr = 0.0; double _prevClose = 0.0; size_t _count = 0; bool _seeded = false;
};

class StreamingRsi
{
public:
    explicit StreamingRsi(size_t period) : _period(period), _gainEma(period), _lossEma(period) {}
    void update(double close)
    {
        if (!_seeded) { _prevClose = close; _seeded = true; return; }
        double diff = close - _prevClose;
        _gainEma.update(diff > 0 ? diff : 0.0);
        _lossEma.update(diff < 0 ? -diff : 0.0);
        _prevClose = close; ++_count;
    }
    double value() const
    {
        if (_count < _period) return std::nan("");
        double g = _gainEma.value(), l = _lossEma.value();
        if (l < 1e-10) return 100.0;
        return 100.0 - 100.0 / (1.0 + g / l);
    }
    bool ready() const { return _count >= _period; }
    void reset() { _gainEma.reset(); _lossEma.reset(); _prevClose = 0.0; _seeded = false; _count = 0; }
private:
    size_t _period; StreamingEma _gainEma, _lossEma; double _prevClose = 0.0; size_t _count = 0; bool _seeded = false;
};

class StreamingStddev
{
public:
    explicit StreamingStddev(size_t period) : _sma(period) {}
    void update(double v) { _sma.update(v); }
    double value() const
    {
        if (!_sma.ready()) return std::nan("");
        double mean = _sma.value();
        double sum = 0.0;
        for (double v : _sma.buf()) { double d = v - mean; sum += d * d; }
        return std::sqrt(sum / static_cast<double>(_sma.buf().size()));
    }
    bool ready() const { return _sma.ready(); }
    void reset() { _sma.reset(); }
private:
    StreamingSma _sma;
};

class StreamingAdx
{
public:
    explicit StreamingAdx(size_t period) : _period(period), _plusDm(period), _minusDm(period), _tr(period), _adx(period) {}
    void update(double high, double low, double close)
    {
        if (!_seeded) { _prevHigh = high; _prevLow = low; _prevClose = close; _seeded = true; return; }
        double tr = std::max({high - low, std::abs(high - _prevClose), std::abs(low - _prevClose)});
        double plusDm = (high - _prevHigh > _prevLow - low && high - _prevHigh > 0) ? high - _prevHigh : 0.0;
        double minusDm = (_prevLow - low > high - _prevHigh && _prevLow - low > 0) ? _prevLow - low : 0.0;
        _tr.update(tr); _plusDm.update(plusDm); _minusDm.update(minusDm);
        if (_tr.ready()) {
            double atr = _tr.value();
            if (atr > 1e-10) {
                double plusDi = 100.0 * _plusDm.value() / atr;
                double minusDi = 100.0 * _minusDm.value() / atr;
                double dx = (plusDi + minusDi > 1e-10) ? 100.0 * std::abs(plusDi - minusDi) / (plusDi + minusDi) : 0.0;
                _adx.update(dx);
            }
        }
        _prevHigh = high; _prevLow = low; _prevClose = close;
    }
    double value() const { return _adx.ready() ? _adx.value() : std::nan(""); }
    bool ready() const { return _adx.ready(); }
    void reset() { _tr.reset(); _plusDm.reset(); _minusDm.reset(); _adx.reset(); _seeded = false; }
private:
    size_t _period; StreamingEma _plusDm, _minusDm, _tr, _adx;
    double _prevHigh = 0, _prevLow = 0, _prevClose = 0; bool _seeded = false;
};

// =====================================================================
// 1. BOLLINGER BREAKOUT
// Python grid: bb_period=[10,20,30,50], bb_std=[1.5,2.0,2.5,3.0], vol_mult=[0.0,1.3,1.5]
// Entry: close > upper BB (prior bar inside). Exit: midline cross + ATR trail.
// =====================================================================
struct BollingerBreakoutParams
{
    int bbPeriod = 20;
    double bbStd = 2.0;
    double volMult = 0.0;    // 0 = disabled
    std::string toString() const {
        return "bb=" + std::to_string(bbPeriod) + ",std=" + std::to_string(bbStd) + ",vm=" + std::to_string(volMult);
    }
};

class BollingerBreakoutStrategy : public Strategy
{
public:
    BollingerBreakoutStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, BollingerBreakoutParams p = {})
        : Strategy(id, sym, reg), _p(p), _sma(static_cast<size_t>(p.bbPeriod)),
          _stddev(static_cast<size_t>(p.bbPeriod)), _atr(14), _volSma(20) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        double vol = ev.bar.volume.toDouble();

        _sma.update(c); _stddev.update(c); _atr.update(h, l, c);
        if (_p.volMult > 0) _volSma.update(vol);
        _prevC = _currC; _currC = c;

        if (!_sma.ready() || !_atr.ready()) return;

        double mean = _sma.value(), sd = _stddev.value();
        if (std::isnan(sd)) return;
        double upper = mean + _p.bbStd * sd, lower = mean - _p.bbStd * sd;

        // Volume filter
        bool volOk = true;
        if (_p.volMult > 0 && _volSma.ready()) {
            double avgVol = _volSma.value();
            if (avgVol > 0) volOk = vol > _p.volMult * avgVol;
        }

        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0)
        {
            // Breakout: close outside BB, prior bar inside
            bool prevInside = (!std::isnan(_prevC)) && _prevC <= (mean + _p.bbStd * sd) && _prevC >= (mean - _p.bbStd * sd);
            if (c > upper && prevInside && volOk) {
                emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
                _trail = c - 2.0 * _atr.value();
            } else if (c < lower && prevInside && volOk) {
                emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
                _trail = c + 2.0 * _atr.value();
            }
        }
        else
        {
            double atrVal = _atr.value();
            if (pos.raw() > 0) {
                _trail = std::max(_trail, c - 2.0 * atrVal);
                if (c < mean || c < _trail) emitClosePosition(ev.symbol);
            } else {
                _trail = std::min(_trail, c + 2.0 * atrVal);
                if (c > mean || c > _trail) emitClosePosition(ev.symbol);
            }
        }
    }
private:
    BollingerBreakoutParams _p;
    StreamingSma _sma; StreamingStddev _stddev; StreamingAtr _atr; StreamingSma _volSma;
    double _prevC = std::nan(""), _currC = 0.0, _trail = 0.0;
};

// =====================================================================
// 2. DONCHIAN BREAKOUT
// Python grid: channel_period=[10,15,20,30,40,55], exit_period=[5,8,10,15,20], vol_mult=[0.0,1.3,1.5,2.0]
// Entry: close > highest(channel_period). Exit: exit_channel midline cross.
// =====================================================================
struct DonchianBreakoutParams
{
    int channelPeriod = 20;
    int exitPeriod = 10;
    double volMult = 0.0;
    std::string toString() const {
        return "cp=" + std::to_string(channelPeriod) + ",ep=" + std::to_string(exitPeriod) + ",vm=" + std::to_string(volMult);
    }
};

class DonchianBreakoutStrategy : public Strategy
{
public:
    DonchianBreakoutStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, DonchianBreakoutParams p = {})
        : Strategy(id, sym, reg), _p(p), _volSma(20) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        double vol = ev.bar.volume.toDouble();
        _highs.push_back(h); _lows.push_back(l);
        if (_highs.size() > static_cast<size_t>(_p.channelPeriod)) { _highs.pop_front(); _lows.pop_front(); }
        _exitH.push_back(h); _exitL.push_back(l);
        if (_exitH.size() > static_cast<size_t>(_p.exitPeriod)) { _exitH.pop_front(); _exitL.pop_front(); }
        if (_p.volMult > 0) _volSma.update(vol);

        if (_highs.size() < static_cast<size_t>(_p.channelPeriod)) return;

        double upper = *std::max_element(_highs.begin(), _highs.end());
        double lower = *std::min_element(_lows.begin(), _lows.end());

        bool volOk = true;
        if (_p.volMult > 0 && _volSma.ready()) { double avg = _volSma.value(); if (avg > 0) volOk = vol > _p.volMult * avg; }

        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0)
        {
            if (c > upper && volOk) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (c < lower && volOk) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        else
        {
            // Exit on exit-channel midline
            double exitMid = (*std::max_element(_exitH.begin(), _exitH.end()) + *std::min_element(_exitL.begin(), _exitL.end())) / 2.0;
            if (pos.raw() > 0 && c < exitMid) emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && c > exitMid) emitClosePosition(ev.symbol);
        }
    }
private:
    DonchianBreakoutParams _p;
    std::deque<double> _highs, _lows, _exitH, _exitL;
    StreamingSma _volSma;
};

// =====================================================================
// 3. DUAL MOMENTUM
// Python grid: lookback=[20,40,60,120], mom_threshold=[0.0,0.02,0.05], smooth=[1,3,5]
// Entry: smoothed momentum > threshold. Exit: opposite cross.
// =====================================================================
struct DualMomentumParams
{
    int lookback = 40;
    double momThreshold = 0.0;
    int smooth = 1;
    std::string toString() const {
        return "lb=" + std::to_string(lookback) + ",mt=" + std::to_string(momThreshold) + ",sm=" + std::to_string(smooth);
    }
};

class DualMomentumStrategy : public Strategy
{
public:
    DualMomentumStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, DualMomentumParams p = {})
        : Strategy(id, sym, reg), _p(p), _ema(20) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_p.lookback + _p.smooth + 2))
            _closes.pop_front();
        _ema.update(c);

        if (_closes.size() < static_cast<size_t>(_p.lookback + 1)) return;
        if (!_ema.ready()) return;

        double mom = _closes.back() / _closes.front() - 1.0;

        // Smooth momentum with EMA of returns
        _momBuf.push_back(mom);
        if (static_cast<int>(_momBuf.size()) > _p.smooth * 2) _momBuf.pop_front();
        double smoothed = mom;
        if (_p.smooth > 1 && _momBuf.size() >= static_cast<size_t>(_p.smooth)) {
            double s = 0.0;
            size_t n = std::min(_momBuf.size(), static_cast<size_t>(_p.smooth));
            for (size_t i = _momBuf.size() - n; i < _momBuf.size(); ++i) s += _momBuf[i];
            smoothed = s / static_cast<double>(n);
        }

        double trend = _ema.value();
        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0) {
            if (smoothed > _p.momThreshold && c > trend) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (smoothed < -_p.momThreshold && c < trend) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        } else {
            if (pos.raw() > 0 && (smoothed < -_p.momThreshold || c < trend)) emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && (smoothed > _p.momThreshold || c > trend)) emitClosePosition(ev.symbol);
        }
    }
private:
    DualMomentumParams _p;
    std::deque<double> _closes, _momBuf;
    StreamingEma _ema;
};

// =====================================================================
// 4. EMA CROSSOVER
// Python grid: fast=[5,8,13,21,34], slow=[21,34,55,89,144], adx_min=[0,15,20,25]
// Entry: fast EMA crosses slow EMA, ADX >= adx_min. Exit: opposite cross.
// =====================================================================
struct EmaCrossoverParams
{
    int fastPeriod = 8;
    int slowPeriod = 21;
    int adxMin = 0;
    std::string toString() const {
        return "f=" + std::to_string(fastPeriod) + ",s=" + std::to_string(slowPeriod) + ",adx=" + std::to_string(adxMin);
    }
};

class EmaCrossoverStrategy : public Strategy
{
public:
    EmaCrossoverStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, EmaCrossoverParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _fast(static_cast<size_t>(p.fastPeriod)), _slow(static_cast<size_t>(p.slowPeriod)), _adx(14) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        _fast.update(c); _slow.update(c);
        if (_p.adxMin > 0) _adx.update(h, l, c);

        if (!_fast.ready() || !_slow.ready()) return;
        if (_p.adxMin > 0 && !_adx.ready()) return;

        double f = _fast.value(), s = _slow.value();
        if (std::isnan(f) || std::isnan(s)) return;

        if (_p.adxMin > 0 && _adx.value() < _p.adxMin) { _prevAbove = f > s; return; }

        bool above = f > s;
        Quantity pos = position(ev.symbol);

        if (above && !_prevAbove && pos.raw() <= 0) {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        } else if (!above && _prevAbove && pos.raw() >= 0) {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        _prevAbove = above;
    }
private:
    EmaCrossoverParams _p;
    StreamingEma _fast, _slow; StreamingAdx _adx;
    bool _prevAbove = false;
};

// =====================================================================
// 5. KELTNER BREAKOUT
// Python grid: ema_period=[10,20,30,40,50,60,80], atr_period=[10,14,20,30],
//              atr_mult=[2.0,2.5,2.8,3.0,3.2,3.5], atr_pct_min=[0.0,0.005,0.008]
// Entry: close > upper Keltner. Exit: midline cross.
// =====================================================================
struct KeltnerBreakoutParams
{
    int emaPeriod = 20;
    int atrPeriod = 14;
    double atrMult = 2.5;
    double atrPctMin = 0.0;
    std::string toString() const {
        return "ema=" + std::to_string(emaPeriod) + ",atr=" + std::to_string(atrPeriod) +
               ",am=" + std::to_string(atrMult) + ",apm=" + std::to_string(atrPctMin);
    }
};

class KeltnerBreakoutStrategy : public Strategy
{
public:
    KeltnerBreakoutStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, KeltnerBreakoutParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _ema(static_cast<size_t>(p.emaPeriod)), _atr(static_cast<size_t>(p.atrPeriod)), _ema50(50) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        _ema.update(c); _atr.update(h, l, c); _ema50.update(c);
        if (!_ema.ready() || !_atr.ready()) return;

        double mid = _ema.value(), atrVal = _atr.value();
        double upper = mid + _p.atrMult * atrVal, lower = mid - _p.atrMult * atrVal;

        // ATR% filter: reject low-vol periods
        if (_p.atrPctMin > 0 && mid > 0 && (atrVal / mid) < _p.atrPctMin) return;

        Quantity pos = position(ev.symbol);
        if (pos.raw() == 0) {
            if (c > upper) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (c < lower) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        } else {
            if (pos.raw() > 0 && c < mid) emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && c > mid) emitClosePosition(ev.symbol);
        }
    }
private:
    KeltnerBreakoutParams _p;
    StreamingEma _ema, _ema50; StreamingAtr _atr;
};

// =====================================================================
// 6. KELTNER SQUEEZE
// Python grid: ema_period=[10,20,30], kelt_mult=[1.5,2.0,2.5], bb_period=[10,20,30], bb_std=[1.5,2.0,2.5]
// Entry: squeeze ON then release + breakout direction. Exit: momentum reversal.
// =====================================================================
struct KeltnerSqueezeParams
{
    int emaPeriod = 20;
    double keltMult = 2.0;
    int bbPeriod = 20;
    double bbStd = 2.0;
    std::string toString() const {
        return "ke=" + std::to_string(emaPeriod) + ",km=" + std::to_string(keltMult) +
               ",bp=" + std::to_string(bbPeriod) + ",bs=" + std::to_string(bbStd);
    }
};

class KeltnerSqueezeStrategy : public Strategy
{
public:
    KeltnerSqueezeStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, KeltnerSqueezeParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _bbSma(static_cast<size_t>(p.bbPeriod)), _bbStd(static_cast<size_t>(p.bbPeriod)),
          _kema(static_cast<size_t>(p.emaPeriod)), _katr(static_cast<size_t>(p.emaPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        _bbSma.update(c); _bbStd.update(c); _kema.update(c); _katr.update(h, l, c);
        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_p.bbPeriod)) _closes.pop_front();

        if (!_bbSma.ready() || !_kema.ready() || !_katr.ready()) return;

        // BB
        double bbMid = _bbSma.value(), sd = _bbStd.value();
        if (std::isnan(sd)) return;
        double bbUp = bbMid + _p.bbStd * sd, bbLo = bbMid - _p.bbStd * sd;

        // Keltner
        double kMid = _kema.value(), atrVal = _katr.value();
        double kUp = kMid + _p.keltMult * atrVal, kLo = kMid - _p.keltMult * atrVal;

        bool squeezing = (bbLo > kLo) && (bbUp < kUp);

        // Momentum oscillator: close - SMA
        double mom = c - bbMid;

        Quantity pos = position(ev.symbol);

        // Enter on squeeze release + directional breakout
        if (!_wasSqueeze && _prevSqueeze && pos.raw() == 0) {
            if (c > bbUp && mom > 0) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (c < bbLo && mom < 0) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        // Exit on re-squeeze or momentum reversal
        else if (pos.raw() > 0 && (squeezing || mom < 0)) emitClosePosition(ev.symbol);
        else if (pos.raw() < 0 && (squeezing || mom > 0)) emitClosePosition(ev.symbol);

        _prevSqueeze = _wasSqueeze;
        _wasSqueeze = squeezing;
    }
private:
    KeltnerSqueezeParams _p;
    std::deque<double> _closes;
    StreamingSma _bbSma; StreamingStddev _bbStd; StreamingEma _kema; StreamingAtr _katr;
    bool _wasSqueeze = false, _prevSqueeze = false;
};

// =====================================================================
// 7. MACD
// Python grid: fast=[8,12,16], slow=[21,26,34], signal_period=[7,9,12], trend_period=[0,100,200]
// Entry: MACD crosses signal + histogram expansion. Exit: opposite cross.
// =====================================================================
struct MacdParams
{
    int fast = 12;
    int slow = 26;
    int signalPeriod = 9;
    int trendPeriod = 0;  // 0 = disabled
    std::string toString() const {
        return "f=" + std::to_string(fast) + ",s=" + std::to_string(slow) +
               ",sig=" + std::to_string(signalPeriod) + ",tp=" + std::to_string(trendPeriod);
    }
};

class MacdStrategy : public Strategy
{
public:
    MacdStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, MacdParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _fastEma(static_cast<size_t>(p.fast)), _slowEma(static_cast<size_t>(p.slow)),
          _sigEma(static_cast<size_t>(p.signalPeriod)),
          _trendEma(static_cast<size_t>(std::max(p.trendPeriod, 1))) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _fastEma.update(c); _slowEma.update(c);
        if (_p.trendPeriod > 0) _trendEma.update(c);

        if (!_fastEma.ready() || !_slowEma.ready()) return;

        double macdLine = _fastEma.value() - _slowEma.value();
        _sigEma.update(macdLine);
        if (!_sigEma.ready()) return;

        double signalLine = _sigEma.value();
        double histogram = macdLine - signalLine;

        // Trend filter
        if (_p.trendPeriod > 0 && _trendEma.ready()) {
            double trend = _trendEma.value();
            Quantity pos = position(ev.symbol);
            if (pos.raw() == 0 && ((histogram > 0 && c < trend) || (histogram < 0 && c > trend)))
                return;
        }

        Quantity pos = position(ev.symbol);
        bool above = histogram > 0;

        if (above && !_prevAbove && pos.raw() <= 0) {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        } else if (!above && _prevAbove && pos.raw() >= 0) {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        _prevAbove = above;
    }
private:
    MacdParams _p;
    StreamingEma _fastEma, _slowEma, _sigEma, _trendEma;
    bool _prevAbove = false;
};

// =====================================================================
// 8. RSI + BOLLINGER BAND MEAN REVERSION
// Python grid: rsi_period=[7,14,21], rsi_low=[20,25,30], rsi_high=[70,75,80],
//              bb_period=[15,20,30], bb_std=[1.5,2.0,2.5]
// Entry: RSI<low + close<lowerBB (long). Exit: return to midline.
// =====================================================================
struct RsiBbMrParams
{
    int rsiPeriod = 14;
    double rsiLow = 30.0;
    double rsiHigh = 70.0;
    int bbPeriod = 20;
    double bbStd = 2.0;
    std::string toString() const {
        return "rp=" + std::to_string(rsiPeriod) + ",rl=" + std::to_string(rsiLow) +
               ",bp=" + std::to_string(bbPeriod) + ",bs=" + std::to_string(bbStd);
    }
};

class RsiBbMrStrategy : public Strategy
{
public:
    RsiBbMrStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, RsiBbMrParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _rsi(static_cast<size_t>(p.rsiPeriod)),
          _bbSma(static_cast<size_t>(p.bbPeriod)), _bbStd(static_cast<size_t>(p.bbPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _rsi.update(c); _bbSma.update(c); _bbStd.update(c);
        if (!_rsi.ready() || !_bbSma.ready()) return;

        double mean = _bbSma.value(), sd = _bbStd.value();
        if (std::isnan(sd)) return;
        double bbUp = mean + _p.bbStd * sd, bbLo = mean - _p.bbStd * sd;
        double rsi = _rsi.value();

        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0) {
            if (c < bbLo && rsi < _p.rsiLow) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (c > bbUp && rsi > _p.rsiHigh) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        } else {
            if (pos.raw() > 0 && c >= mean) emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && c <= mean) emitClosePosition(ev.symbol);
        }
    }
private:
    RsiBbMrParams _p;
    StreamingRsi _rsi; StreamingSma _bbSma; StreamingStddev _bbStd;
};

// =====================================================================
// 9. RSI-2 (Larry Connors)
// Python grid: rsi_period=[2,3], entry_low=[5,10,15], entry_high=[85,90,95], trend_period=[50,100,200]
// Entry: RSI(rsi_period) < entry_low AND price > SMA(trend). Exit: RSI > 50.
// =====================================================================
struct Rsi2Params
{
    int rsiPeriod = 2;
    double entryLow = 10.0;
    double entryHigh = 90.0;
    int trendPeriod = 200;
    std::string toString() const {
        return "rp=" + std::to_string(rsiPeriod) + ",el=" + std::to_string(entryLow) +
               ",eh=" + std::to_string(entryHigh) + ",tp=" + std::to_string(trendPeriod);
    }
};

class Rsi2Strategy : public Strategy
{
public:
    Rsi2Strategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, Rsi2Params p = {})
        : Strategy(id, sym, reg), _p(p),
          _rsi(static_cast<size_t>(p.rsiPeriod)), _sma(static_cast<size_t>(p.trendPeriod)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double c = ev.bar.close.toDouble();
        _rsi.update(c); _sma.update(c);
        if (!_rsi.ready() || !_sma.ready()) return;

        double rsi = _rsi.value(), trend = _sma.value();
        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0) {
            if (rsi < _p.entryLow && c > trend) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (rsi > _p.entryHigh && c < trend) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        } else {
            if (pos.raw() > 0 && rsi > 50.0) emitClosePosition(ev.symbol);
            else if (pos.raw() < 0 && rsi < 50.0) emitClosePosition(ev.symbol);
        }
    }
private:
    Rsi2Params _p;
    StreamingRsi _rsi; StreamingSma _sma;
};

// =====================================================================
// 10. SUPERTREND
// Python grid: atr_period=[7,10,14,20], atr_mult=[2.0,2.5,3.0,3.5,4.0], adx_min=[0,20,25]
// Entry: direction flip. Exit: opposite flip.
// =====================================================================
struct SupertrendParams
{
    int atrPeriod = 10;
    double atrMult = 3.0;
    int adxMin = 0;
    std::string toString() const {
        return "ap=" + std::to_string(atrPeriod) + ",am=" + std::to_string(atrMult) + ",adx=" + std::to_string(adxMin);
    }
};

class SupertrendStrategy : public Strategy
{
public:
    SupertrendStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, SupertrendParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _atr(static_cast<size_t>(p.atrPeriod)), _adx(14) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        _atr.update(h, l, c);
        if (_p.adxMin > 0) _adx.update(h, l, c);
        if (!_atr.ready()) return;

        double atrVal = _atr.value(), hl2 = (h + l) / 2.0;
        double rawUp = hl2 + _p.atrMult * atrVal, rawLo = hl2 - _p.atrMult * atrVal;

        if (std::isnan(_upper) || rawUp < _upper || _prevC > _upper) _upper = rawUp;
        if (std::isnan(_lower) || rawLo > _lower || _prevC < _lower) _lower = rawLo;

        int dir;
        if (_prevDir == -1) dir = (c > _upper) ? 1 : -1;
        else dir = (c < _lower) ? -1 : (_prevDir == 0 ? 1 : _prevDir);

        // ADX filter
        if (_p.adxMin > 0 && _adx.ready() && _adx.value() < _p.adxMin) { _prevC = c; _prevDir = dir; return; }

        Quantity pos = position(ev.symbol);
        if (dir == 1 && _prevDir == -1 && pos.raw() <= 0) {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        } else if (dir == -1 && _prevDir == 1 && pos.raw() >= 0) {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
        _prevC = c; _prevDir = dir;
    }
private:
    SupertrendParams _p;
    StreamingAtr _atr; StreamingAdx _adx;
    double _upper = std::nan(""), _lower = std::nan(""), _prevC = 0.0;
    int _prevDir = 0;
};

// =====================================================================
// 11. TREND PULLBACK
// Python grid: fast_ema=[10,20,30], slow_ema=[50,100,150,200], rsi_pullback=[35,40,45], atr_pct_min=[0.0,0.004,0.008]
// Entry: uptrend + RSI pullback + reclaim fast EMA. Exit: chandelier.
// =====================================================================
struct TrendPullbackParams
{
    int fastEma = 20;
    int slowEma = 100;
    int rsiPullback = 40;
    double atrPctMin = 0.0;
    std::string toString() const {
        return "fe=" + std::to_string(fastEma) + ",se=" + std::to_string(slowEma) +
               ",rp=" + std::to_string(rsiPullback) + ",apm=" + std::to_string(atrPctMin);
    }
};

class TrendPullbackStrategy : public Strategy
{
public:
    TrendPullbackStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, TrendPullbackParams p = {})
        : Strategy(id, sym, reg), _p(p),
          _fast(static_cast<size_t>(p.fastEma)), _slow(static_cast<size_t>(p.slowEma)),
          _rsi(14), _atr(14) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        _fast.update(c); _slow.update(c); _rsi.update(c); _atr.update(h, l, c);
        if (!_fast.ready() || !_slow.ready() || !_rsi.ready() || !_atr.ready()) return;

        double fast = _fast.value(), slow = _slow.value();
        double rsi = _rsi.value(), atrVal = _atr.value();

        // ATR% filter
        if (_p.atrPctMin > 0 && c > 0 && (atrVal / c) < _p.atrPctMin) return;

        Quantity pos = position(ev.symbol);
        bool uptrend = c > slow;

        if (pos.raw() == 0) {
            // Long: uptrend + RSI pulled back + price reclaims fast EMA
            if (uptrend && rsi < _p.rsiPullback && c > fast) {
                emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
                _trail = c - 3.0 * atrVal;
            }
            // Short: downtrend + RSI overbought + price loses fast EMA
            else if (!uptrend && rsi > (100 - _p.rsiPullback) && c < fast) {
                emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
                _trail = c + 3.0 * atrVal;
            }
        } else {
            // Chandelier exit: trailing stop
            if (pos.raw() > 0) {
                _trail = std::max(_trail, h - 3.0 * atrVal);
                if (c < _trail) emitClosePosition(ev.symbol);
            } else {
                _trail = std::min(_trail, l + 3.0 * atrVal);
                if (c > _trail) emitClosePosition(ev.symbol);
            }
        }
    }
private:
    TrendPullbackParams _p;
    StreamingEma _fast, _slow; StreamingRsi _rsi; StreamingAtr _atr;
    double _trail = 0.0;
};

// =====================================================================
// 12. TIME-SERIES MOMENTUM (TSMOM)
// Python grid: lookback=[20,40,60,80,120,160], smooth=[1,3,5,10], atr_pct_min=[0.0,0.005,0.008]
// Entry: sign(returns). Exit: sign flip.
// =====================================================================
struct TsmomParams
{
    int lookback = 60;
    int smooth = 1;
    double atrPctMin = 0.0;
    std::string toString() const {
        return "lb=" + std::to_string(lookback) + ",sm=" + std::to_string(smooth) + ",apm=" + std::to_string(atrPctMin);
    }
};

class TsmomStrategy : public Strategy
{
public:
    TsmomStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, TsmomParams p = {})
        : Strategy(id, sym, reg), _p(p), _atr(20) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        _closes.push_back(c);
        if (_closes.size() > static_cast<size_t>(_p.lookback + 2)) _closes.pop_front();
        _atr.update(h, l, c);

        if (_closes.size() < static_cast<size_t>(_p.lookback + 1)) return;

        double mom = _closes.back() / _closes.front() - 1.0;

        // Smooth
        _momBuf.push_back(mom);
        if (static_cast<int>(_momBuf.size()) > _p.smooth * 2) _momBuf.pop_front();
        double smoothed = mom;
        if (_p.smooth > 1 && _momBuf.size() >= static_cast<size_t>(_p.smooth)) {
            double s = 0.0;
            size_t n = std::min(_momBuf.size(), static_cast<size_t>(_p.smooth));
            for (size_t i = _momBuf.size() - n; i < _momBuf.size(); ++i) s += _momBuf[i];
            smoothed = s / static_cast<double>(n);
        }

        // ATR% filter
        if (_p.atrPctMin > 0 && _atr.ready() && c > 0) {
            if ((_atr.value() / c) < _p.atrPctMin) return;
        }

        Quantity pos = position(ev.symbol);
        if (smoothed > 0 && pos.raw() <= 0) {
            if (pos.raw() < 0) emitClosePosition(ev.symbol);
            emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
        } else if (smoothed < 0 && pos.raw() >= 0) {
            if (pos.raw() > 0) emitClosePosition(ev.symbol);
            emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        }
    }
private:
    TsmomParams _p;
    std::deque<double> _closes, _momBuf;
    StreamingAtr _atr;
};

// =====================================================================
// 13. VOL COMPRESSION BREAKOUT
// Python grid: range_window=[20,30,40,60], compression_window=[60,100,150],
//              compression_pct=[0.2,0.3,0.4], vol_mult=[0.0,1.3,1.5]
// Entry: ATR% rank low (compressed) then close breaks range_window high/low. Exit: opposite.
// =====================================================================
struct VolCompressionBreakoutParams
{
    int rangeWindow = 30;
    int compressionWindow = 100;
    double compressionPct = 0.3;
    double volMult = 0.0;
    std::string toString() const {
        return "rw=" + std::to_string(rangeWindow) + ",cw=" + std::to_string(compressionWindow) +
               ",cp=" + std::to_string(compressionPct) + ",vm=" + std::to_string(volMult);
    }
};

class VolCompressionBreakoutStrategy : public Strategy
{
public:
    VolCompressionBreakoutStrategy(SubscriberId id, SymbolId sym, const SymbolRegistry& reg, VolCompressionBreakoutParams p = {})
        : Strategy(id, sym, reg), _p(p), _atr(14), _volSma(20) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
    {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        double vol = ev.bar.volume.toDouble();
        _atr.update(h, l, c);
        if (_p.volMult > 0) _volSma.update(vol);

        _highs.push_back(h); _lows.push_back(l);
        if (_highs.size() > static_cast<size_t>(_p.rangeWindow)) { _highs.pop_front(); _lows.pop_front(); }
        _atrPcts.push_back(c > 0 ? _atr.value() / c : 0.0);
        if (_atrPcts.size() > static_cast<size_t>(_p.compressionWindow)) _atrPcts.pop_back();

        if (!_atr.ready()) return;
        if (_highs.size() < static_cast<size_t>(_p.rangeWindow)) return;

        // ATR% rank: percentile of current ATR% in compression window
        bool compressed = false;
        if (_atrPcts.size() >= static_cast<size_t>(_p.compressionWindow)) {
            double curAtrPct = _atrPcts.back();
            size_t rank = 0;
            for (double v : _atrPcts) if (v <= curAtrPct) ++rank;
            double percentile = static_cast<double>(rank) / static_cast<double>(_atrPcts.size());
            compressed = percentile <= _p.compressionPct;
        }

        bool volOk = true;
        if (_p.volMult > 0 && _volSma.ready()) { double avg = _volSma.value(); if (avg > 0) volOk = vol > _p.volMult * avg; }

        double rangeHigh = *std::max_element(_highs.begin(), _highs.end());
        double rangeLow = *std::min_element(_lows.begin(), _lows.end());

        Quantity pos = position(ev.symbol);

        if (pos.raw() == 0 && compressed) {
            if (c > rangeHigh && volOk) emitMarketBuy(ev.symbol, Quantity::fromDouble(1.0));
            else if (c < rangeLow && volOk) emitMarketSell(ev.symbol, Quantity::fromDouble(1.0));
        } else if (pos.raw() > 0 && c < rangeLow) {
            emitClosePosition(ev.symbol);
        } else if (pos.raw() < 0 && c > rangeHigh) {
            emitClosePosition(ev.symbol);
        }
    }
private:
    VolCompressionBreakoutParams _p;
    std::deque<double> _highs, _lows;
    std::vector<double> _atrPcts;
    StreamingAtr _atr; StreamingSma _volSma;
};

}  // namespace flox::strategy
