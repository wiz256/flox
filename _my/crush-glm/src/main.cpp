#include "csv_reader.h"
#include "flox/aggregator/bar.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_tracker.h"
#include "flox/strategy/strategy.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace flox;
using namespace crush;

constexpr double FEE_RATE = 0.0004;
constexpr double INITIAL_CAPITAL = 10000.0;

enum class SizingMode : uint8_t { AllEquity, FixedNotional };
inline Quantity qtyForTrade(double price, SizingMode mode) {
    if (price <= 0.0) return Quantity::fromDouble(0.0);
    return Quantity::fromDouble(mode == SizingMode::AllEquity ? INITIAL_CAPITAL / price : 1000.0 / price);
}

enum class ExitMode : uint8_t { Chandelier, ChandelierTimeStop, SignalBETrail };

struct Params {
    std::string symbol;
    int signal_p1 = 20, signal_p2 = 50;
    double signal_p3 = 2.0;
    ExitMode exit_mode = ExitMode::Chandelier;
    int exit_atr_period = 14;
    double exit_sl_mult = 2.0, exit_trail_mult = 3.0;
    int exit_time_bars = 0;
    double exit_be_r_mult = 1.0;
};

struct GridResult {
    Params params;
    double sharpe = 0;
    size_t trades = 0;
    double totalReturn = 0;
    double maxDrawdownPct = 0;
    double winRate = 0;
};

// ── Indicators ──────────────────────────────────────────────────────────
class StreamEma {
    double mult_; double val_ = 0; size_t n_ = 0; bool seeded_ = false;
public:
    explicit StreamEma(size_t p) : mult_(2.0 / (p + 1.0)) {}
    void update(double v) { if (!seeded_) { val_ = v; seeded_ = true; return; } val_ = (v - val_) * mult_ + val_; ++n_; }
    double value() const { return seeded_ ? val_ : 0; }
    bool ready() const { return n_ >= 1; }
};
class StreamSma {
    size_t p_; std::deque<double> buf_;
public:
    explicit StreamSma(size_t p) : p_(p) {}
    void update(double v) { buf_.push_back(v); if (buf_.size() > p_) buf_.pop_front(); }
    double value() const { if (buf_.size() < p_) return 0; double s = 0; for (double v : buf_) s += v; return s / buf_.size(); }
    bool ready() const { return buf_.size() >= p_; }
    const std::deque<double>& buf() const { return buf_; }
};
class StreamAtr {
    size_t p_, n_ = 0; double val_ = 0, prevC_ = 0; bool seeded_ = false;
public:
    explicit StreamAtr(size_t p) : p_(p) {}
    void update(double h, double l, double c) {
        double tr = std::max({h - l, std::abs(h - prevC_), std::abs(l - prevC_)});
        if (!seeded_) { val_ = tr; seeded_ = true; } else { val_ = (val_ * (p_ - 1) + tr) / p_; }
        prevC_ = c; ++n_;
    }
    double value() const { return n_ >= p_ ? val_ : 0; }
    bool ready() const { return n_ >= p_; }
};
class StreamRsi {
    StreamEma gain_, loss_; size_t n_ = 0; double prevC_ = 0; bool seeded_ = false;
public:
    explicit StreamRsi(size_t p) : gain_(p), loss_(p) {}
    void update(double c) {
        if (!seeded_) { prevC_ = c; seeded_ = true; return; }
        double d = c - prevC_;
        gain_.update(d > 0 ? d : 0); loss_.update(d < 0 ? -d : 0);
        prevC_ = c; ++n_;
    }
    double value() const { if (n_ < 1) return 50; double g = gain_.value(), lo = loss_.value(); return lo < 1e-10 ? 100 : 100 - 100 / (1 + g / lo); }
    bool ready() const { return n_ >= 1; }
};
class StreamStddev {
    StreamSma sma_;
public:
    explicit StreamStddev(size_t p) : sma_(p) {}
    void update(double v) { sma_.update(v); }
    double value() const {
        if (!sma_.ready()) return 0;
        double m = sma_.value(); double s = 0;
        for (double v : sma_.buf()) { double d = v - m; s += d * d; }
        return std::sqrt(s / sma_.buf().size());
    }
    bool ready() const { return sma_.ready(); }
};

// ── Base Strategy ──────────────────────────────────────────────────────
// KEY FIX: Check risk exits (chandelier/SL/trail) BEFORE signal exits.
// In v3, signal exits fired first, making all 3 exit modes identical.
// Now: risk exit → signal exit → signal entry, all on same bar.
class BaseStrategy : public Strategy {
public:
    BaseStrategy(SubscriberId sid, SymbolId sym, const SymbolRegistry& reg, const Params& p,
                 SizingMode sizing = SizingMode::FixedNotional)
        : Strategy(sid, sym, reg), p_(p), exitAtr_(p.exit_atr_period), sizing_(sizing) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        closes_.push_back(c); highs_.push_back(h); lows_.push_back(l);
        exitAtr_.update(h, l, c);
        SymbolId sym = symbol();
        Quantity pos = position(sym);

        // ── STEP 1: If in position, check risk exit FIRST ──
        if (pos.raw() != 0 && exitAtr_.ready()) {
            checkRiskExit(sym, c, h, l, exitAtr_.value());
            pos = position(sym); // refresh after potential close
        }

        // ── STEP 2: Generate signal and execute entries/exits ──
        onBarImpl(h, l, c);
    }

    virtual void onBarImpl(double h, double l, double c) = 0;

    void enterLong(SymbolId sym, double price) {
        emitMarketBuy(sym, qtyForTrade(price, sizing_));
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; side_ = 1; beAct_ = false; trail_ = 0;
    }
    void enterShort(SymbolId sym, double price) {
        emitMarketSell(sym, qtyForTrade(price, sizing_));
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; side_ = -1; beAct_ = false; trail_ = 0;
    }

    void checkRiskExit(SymbolId sym, double c, double h, double l, double atr) {
        if (atr <= 0) return;
        double ir = p_.exit_sl_mult * atr;
        barsInTrade_++;

        if (side_ == 1) {
            highSince_ = std::max(highSince_, h);
            if (p_.exit_mode == ExitMode::Chandelier || p_.exit_mode == ExitMode::ChandelierTimeStop) {
                double trail_level = highSince_ - p_.exit_trail_mult * atr;
                double hard_sl = entryPrice_ - ir;
                double level = std::max(trail_level, hard_sl);
                if (l <= level) { emitClosePosition(sym); side_ = 0; return; }
                if (p_.exit_mode == ExitMode::ChandelierTimeStop && p_.exit_time_bars > 0 && barsInTrade_ >= p_.exit_time_bars) {
                    emitClosePosition(sym); side_ = 0; return;
                }
            }
            if (p_.exit_mode == ExitMode::SignalBETrail) {
                if (!beAct_ && (h - entryPrice_) >= p_.exit_be_r_mult * ir) { beAct_ = true; trail_ = entryPrice_; }
                if (beAct_) {
                    trail_ = std::max(trail_, h - p_.exit_trail_mult * atr);
                    if (l <= trail_) { emitClosePosition(sym); side_ = 0; return; }
                }
                if (l <= entryPrice_ - ir) { emitClosePosition(sym); side_ = 0; return; }
            }
        } else if (side_ == -1) {
            lowSince_ = std::min(lowSince_, l);
            if (p_.exit_mode == ExitMode::Chandelier || p_.exit_mode == ExitMode::ChandelierTimeStop) {
                double trail_level = lowSince_ + p_.exit_trail_mult * atr;
                double hard_sl = entryPrice_ + ir;
                double level = std::min(trail_level, hard_sl);
                if (h >= level) { emitClosePosition(sym); side_ = 0; return; }
                if (p_.exit_mode == ExitMode::ChandelierTimeStop && p_.exit_time_bars > 0 && barsInTrade_ >= p_.exit_time_bars) {
                    emitClosePosition(sym); side_ = 0; return;
                }
            }
            if (p_.exit_mode == ExitMode::SignalBETrail) {
                if (!beAct_ && (entryPrice_ - l) >= p_.exit_be_r_mult * ir) { beAct_ = true; trail_ = entryPrice_; }
                if (beAct_) {
                    trail_ = std::min(trail_, l + p_.exit_trail_mult * atr);
                    if (h >= trail_) { emitClosePosition(sym); side_ = 0; return; }
                }
                if (h >= entryPrice_ + ir) { emitClosePosition(sym); side_ = 0; return; }
            }
        }
    }

    const Params p_;
    StreamAtr exitAtr_;
    SizingMode sizing_;
    std::vector<double> closes_, highs_, lows_;
    bool beAct_ = false;
    int side_ = 0, barsInTrade_ = 0;
    double entryPrice_ = 0, highSince_ = 0, lowSince_ = 1e18, trail_ = 0;
};

// ── Strategies ─────────────────────────────────────────────────────────
class DonchianStrat final : public BaseStrategy {
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double c) override {
        size_t w = p_.signal_p1;
        if (closes_.size() < w + 1) return;
        double pu = *std::max_element(highs_.end() - ptrdiff_t(w) - 1, highs_.end() - 1);
        double pl = *std::min_element(lows_.end() - ptrdiff_t(w) - 1, lows_.end() - 1);
        SymbolId sym = symbol();
        if (side_ == 0 && c > pu) enterLong(sym, c);
        else if (side_ == 0 && c < pl) enterShort(sym, c);
        else if (side_ == 1 && c < pl) emitClosePosition(sym);
        else if (side_ == -1 && c > pu) emitClosePosition(sym);
    }
};
class DualMomStrat final : public BaseStrategy {
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double c) override {
        int lb = p_.signal_p1;
        if (int(closes_.size()) < lb + 2) return;
        double m = (closes_.back() - closes_[closes_.size()-1-lb]) / closes_[closes_.size()-1-lb];
        double mp = (closes_[closes_.size()-2] - closes_[closes_.size()-2-lb]) / closes_[closes_.size()-2-lb];
        SymbolId sym = symbol();
        if (side_ == 0 && m > 0 && mp <= 0) enterLong(sym, c);
        else if (side_ == 0 && m < 0 && mp >= 0) enterShort(sym, c);
        else if (side_ == 1 && m <= 0) emitClosePosition(sym);
        else if (side_ == -1 && m >= 0) emitClosePosition(sym);
    }
};
class EmaCrossStrat final : public BaseStrategy {
    StreamEma fast_, slow_; bool prevAbove_ = false;
public:
    EmaCrossStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p, SizingMode sz = SizingMode::FixedNotional)
        : BaseStrategy(sid, s, r, p, sz), fast_(p.signal_p1), slow_(p.signal_p2) {}
    void onBarImpl(double, double, double c) override {
        fast_.update(c); slow_.update(c);
        if (!fast_.ready() || !slow_.ready()) return;
        bool above = fast_.value() > slow_.value();
        SymbolId sym = symbol();
        if (side_ == 0 && above && !prevAbove_) enterLong(sym, c);
        else if (side_ == 0 && !above && prevAbove_) enterShort(sym, c);
        else if (side_ == 1 && !above) emitClosePosition(sym);
        else if (side_ == -1 && above) emitClosePosition(sym);
        prevAbove_ = above;
    }
};
class KeltnerBrkStrat final : public BaseStrategy {
    StreamEma ema_; StreamAtr atr_; double mult_;
public:
    KeltnerBrkStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p, SizingMode sz = SizingMode::FixedNotional)
        : BaseStrategy(sid, s, r, p, sz), ema_(p.signal_p1), atr_(p.signal_p1), mult_(p.signal_p3) {}
    void onBarImpl(double h, double l, double c) override {
        ema_.update(c); atr_.update(h, l, c);
        if (!ema_.ready() || !atr_.ready()) return;
        double mid = ema_.value(), a = atr_.value(), u = mid + mult_ * a, lo = mid - mult_ * a;
        SymbolId sym = symbol();
        if (side_ == 0 && c > u) enterLong(sym, c);
        else if (side_ == 0 && c < lo) enterShort(sym, c);
        else if (side_ == 1 && c < mid) emitClosePosition(sym);
        else if (side_ == -1 && c > mid) emitClosePosition(sym);
    }
};
class SupertrendStrat final : public BaseStrategy {
    StreamAtr atr_; double mult_; double upper_ = 0, lower_ = 0, prevC_ = 0; int prevDir_ = 0; bool init_ = false;
public:
    SupertrendStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p, SizingMode sz = SizingMode::FixedNotional)
        : BaseStrategy(sid, s, r, p, sz), atr_(p.signal_p1), mult_(p.signal_p3) {}
    void onBarImpl(double h, double l, double c) override {
        atr_.update(h, l, c);
        if (!atr_.ready()) return;
        double a = atr_.value(), hl2 = (h + l) / 2.0;
        double rawUp = hl2 + mult_ * a, rawLo = hl2 - mult_ * a;
        if (!init_) { upper_ = rawUp; lower_ = rawLo; init_ = true; prevC_ = c; prevDir_ = 1; return; }
        if (rawUp < upper_ || prevC_ > upper_) upper_ = rawUp;
        if (rawLo > lower_ || prevC_ < lower_) lower_ = rawLo;
        int d = (prevDir_ == -1) ? (c > upper_ ? 1 : -1) : (c < lower_ ? -1 : prevDir_);
        SymbolId sym = symbol();
        if (d == 1 && prevDir_ == -1 && side_ == 0) enterLong(sym, c);
        else if (d == -1 && prevDir_ == 1 && side_ == 0) enterShort(sym, c);
        else if (side_ == 1 && d == -1) emitClosePosition(sym);
        else if (side_ == -1 && d == 1) emitClosePosition(sym);
        prevC_ = c; prevDir_ = d;
    }
};
class TsmomStrat final : public BaseStrategy {
    double prevMom_ = 0;
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double c) override {
        int lb = p_.signal_p1; size_t vw = p_.signal_p2 > 0 ? p_.signal_p2 : 20;
        if (closes_.size() < std::max(size_t(lb), vw) + 2) return;
        double m = (closes_.back() - closes_[closes_.size()-1-lb]) / closes_[closes_.size()-1-lb];
        double sq = 0, mn = 0;
        for (size_t i = closes_.size()-vw; i < closes_.size(); ++i) mn += closes_[i]/closes_[i-1]-1;
        mn /= vw;
        for (size_t i = closes_.size()-vw; i < closes_.size(); ++i) { double r=closes_[i]/closes_[i-1]-1-mn; sq += r*r; }
        double vol = std::sqrt(sq/vw)*std::sqrt(252.0);
        SymbolId sym = symbol();
        if (vol < p_.signal_p3 && side_ == 0 && m > 0 && prevMom_ <= 0) enterLong(sym, c);
        else if (vol < p_.signal_p3 && side_ == 0 && m < 0 && prevMom_ >= 0) enterShort(sym, c);
        else if (side_ == 1 && m <= 0) emitClosePosition(sym);
        else if (side_ == -1 && m >= 0) emitClosePosition(sym);
        prevMom_ = m;
    }
};
class Rsi2Strat final : public BaseStrategy {
    StreamRsi rsi_; StreamSma sma_;
public:
    Rsi2Strat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p, SizingMode sz = SizingMode::FixedNotional)
        : BaseStrategy(sid, s, r, p, sz), rsi_(p.signal_p1), sma_(p.signal_p2) {}
    void onBarImpl(double, double, double c) override {
        rsi_.update(c); sma_.update(c);
        if (!rsi_.ready() || !sma_.ready()) return;
        double rv = rsi_.value(), sv = sma_.value(), os = p_.signal_p3, ob = 100-os;
        SymbolId sym = symbol();
        if (side_ == 0 && rv < os && c > sv) enterLong(sym, c);
        else if (side_ == 0 && rv > ob && c < sv) enterShort(sym, c);
        else if (side_ == 1 && (rv > ob || c < sv)) emitClosePosition(sym);
        else if (side_ == -1 && (rv < os || c > sv)) emitClosePosition(sym);
    }
};
class RsiBbMrStrat final : public BaseStrategy {
    StreamRsi rsi_; StreamSma bbSma_; StreamStddev bbStd_; double bbMult_;
public:
    RsiBbMrStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p, SizingMode sz = SizingMode::FixedNotional)
        : BaseStrategy(sid, s, r, p, sz), rsi_(p.signal_p1), bbSma_(p.signal_p2), bbStd_(p.signal_p2), bbMult_(p.signal_p3) {}
    void onBarImpl(double, double, double c) override {
        rsi_.update(c); bbSma_.update(c); bbStd_.update(c);
        if (!rsi_.ready() || !bbSma_.ready() || !bbStd_.ready()) return;
        double rv = rsi_.value(), mid = bbSma_.value(), sd = bbStd_.value();
        double lo = mid - bbMult_ * sd, hi = mid + bbMult_ * sd;
        SymbolId sym = symbol();
        if (side_ == 0 && rv < 30 && c < lo) enterLong(sym, c);
        else if (side_ == 0 && rv > 70 && c > hi) enterShort(sym, c);
        else if (side_ == 1 && c >= mid) emitClosePosition(sym);
        else if (side_ == -1 && c <= mid) emitClosePosition(sym);
    }
};

// ── Factory & Grid ─────────────────────────────────────────────────────
using Factory = std::function<std::unique_ptr<BaseStrategy>(SubscriberId, SymbolId, const SymbolRegistry&, const Params&, SizingMode)>;
std::vector<std::pair<std::string, Factory>> allStrats() {
    return {
        {"donchian", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<DonchianStrat>(sid, s, r, p, sz); }},
        {"dual_momentum", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<DualMomStrat>(sid, s, r, p, sz); }},
        {"ema_crossover", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<EmaCrossStrat>(sid, s, r, p, sz); }},
        {"keltner_breakout", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<KeltnerBrkStrat>(sid, s, r, p, sz); }},
        {"supertrend", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<SupertrendStrat>(sid, s, r, p, sz); }},
        {"tsmom", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<TsmomStrat>(sid, s, r, p, sz); }},
        {"rsi2", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<Rsi2Strat>(sid, s, r, p, sz); }},
        {"rsi_bb_mr", [](auto sid, auto s, auto& r, auto& p, auto sz) { return std::make_unique<RsiBbMrStrat>(sid, s, r, p, sz); }},
    };
}
struct GridConfig { std::string name; std::vector<int> p1; std::vector<int> p2; std::vector<double> p3; };

// ── Run & Helpers ──────────────────────────────────────────────────────
std::vector<BarEvent> csvToBarEvents(const std::vector<CsvBar>& bars, SymbolId sym) {
    std::vector<BarEvent> events; events.reserve(bars.size());
    for (const auto& b : bars) {
        BarEvent ev; ev.symbol = sym; ev.barType = BarType::Time;
        ev.barTypeParam = 4 * 3600ULL * 1'000'000'000ULL;
        ev.bar.open = Price::fromDouble(b.open); ev.bar.high = Price::fromDouble(b.high);
        ev.bar.low = Price::fromDouble(b.low); ev.bar.close = Price::fromDouble(b.close);
        ev.bar.volume = Volume::fromDouble(b.volume);
        ev.bar.startTime = TimePoint{} + std::chrono::nanoseconds(b.timestamp_ms * 1'000'000LL - 14'400'000'000'000LL);
        ev.bar.endTime = TimePoint{} + std::chrono::nanoseconds(b.timestamp_ms * 1'000'000LL);
        events.push_back(ev);
    }
    return events;
}

struct RunMetrics {
    double sharpe = 0, totalReturn = 0, maxDrawdownPct = 0, winRate = 0;
    size_t trades = 0;
    std::vector<double> barReturns;
};
RunMetrics runSingle(const Factory& fac, SymbolId sid, const SymbolRegistry& reg,
                     const Params& p, const std::vector<BarEvent>& events, SizingMode sizing) {
    RunMetrics rm;
    try {
        auto strat = fac(1, sid, reg, p, sizing);
        PositionTracker posTracker(1);
        BacktestConfig cfg; cfg.initialCapital = INITIAL_CAPITAL; cfg.feeRate = FEE_RATE;
        BacktestRunner runner(cfg);
        runner.setStrategy(strat.get());
        runner.addExecutionListener(&posTracker);
        strat->setPositionManager(&posTracker);
        auto r = runner.runBars(events);
        auto s = r.computeStats();
        rm.sharpe = s.sharpeRatio; rm.totalReturn = s.totalPnl;
        rm.maxDrawdownPct = s.maxDrawdownPct; rm.winRate = s.winRate; rm.trades = s.totalTrades;
        auto eq = r.equityCurve();
        rm.barReturns.reserve(eq.size());
        for (size_t i = 1; i < eq.size(); ++i) {
            double prev = eq[i-1].equity;
            if (prev > 0) rm.barReturns.push_back((eq[i].equity - prev) / prev);
        }
    } catch (...) {}
    return rm;
}

struct PlateauResult { bool isPlateau = false; int neighborCount = 0; double neighborRatio = 0; double relVariance = 1.0; };
PlateauResult detectPlateau(const std::vector<GridResult>& results, size_t bestIdx, const GridConfig& gc) {
    PlateauResult pr;
    if (bestIdx >= results.size() || results[bestIdx].sharpe <= 0) return pr;
    const auto& best = results[bestIdx]; double bestSharpe = best.sharpe;
    auto p1R = gc.p1.empty() ? 1.0 : double(gc.p1.back() - gc.p1.front());
    auto p2R = gc.p2.empty() ? 1.0 : double(gc.p2.back() - gc.p2.front());
    double p3R = gc.p3.empty() ? 1.0 : (gc.p3.back() - gc.p3.front());
    if (p1R <= 0) p1R = 1; if (p2R <= 0) p2R = 1; if (p3R <= 0) p3R = 1;
    std::vector<double> ns;
    for (size_t i = 0; i < results.size(); ++i) {
        if (i == bestIdx) continue;
        double d1 = std::abs(results[i].params.signal_p1 - best.params.signal_p1) / p1R;
        bool sP2 = gc.p2.size() <= 1 || gc.p2[0] == 0;
        double d2 = sP2 ? 0 : std::abs(results[i].params.signal_p2 - best.params.signal_p2) / p2R;
        bool sP3 = gc.p3.size() <= 1 || gc.p3[0] == 0;
        double d3 = sP3 ? 0 : std::abs(results[i].params.signal_p3 - best.params.signal_p3) / p3R;
        if (d1 <= 0.3 && d2 <= 0.3 && d3 <= 0.3) ns.push_back(results[i].sharpe);
    }
    pr.neighborCount = ns.size();
    if (pr.neighborCount < 3) return pr;
    int above = 0; for (double s : ns) if (s >= bestSharpe * 0.8) above++;
    pr.neighborRatio = double(above) / pr.neighborCount;
    double mean = std::accumulate(ns.begin(), ns.end(), 0.0) / pr.neighborCount;
    double sq = 0; for (double s : ns) { double d = s - mean; sq += d * d; }
    pr.relVariance = std::sqrt(sq / pr.neighborCount) / std::abs(bestSharpe);
    pr.isPlateau = pr.neighborRatio >= 0.5 && pr.relVariance < 0.15;
    return pr;
}

struct WFResult { double avgOosSharpe = 0; double oosIsRatio = 0; bool passed = false; int validFolds = 0; };
WFResult walkForward(const Factory& fac, SymbolId sid, const SymbolRegistry& reg,
                     const Params& p, const std::vector<BarEvent>& events,
                     int nFolds, double trainPct, int purgeBars, int minIs, int minOos, SizingMode sizing) {
    WFResult wf; size_t n = events.size();
    size_t ws = n / nFolds, ts = size_t(ws * trainPct), pg = std::min(size_t(purgeBars), ws / 10);
    if (ws < 50 || ts < 30) return wf;
    double tIs = 0, tOos = 0; int vf = 0, fp = 0;
    for (int f = 0; f < nFolds; ++f) {
        size_t off = f * ws, te = off + ts, tst = te + pg, tend = std::min(off + ws, n);
        if (tst >= tend || tst >= n) continue;
        auto isR = runSingle(fac, sid, reg, p, {events.begin()+off, events.begin()+ptrdiff_t(te)}, sizing);
        auto oosR = runSingle(fac, sid, reg, p, {events.begin()+ptrdiff_t(tst), events.begin()+ptrdiff_t(tend)}, sizing);
        if (isR.trades >= size_t(minIs) && oosR.trades >= size_t(minOos)) {
            tIs += isR.sharpe; tOos += oosR.sharpe; vf++;
            if (oosR.sharpe > 0) fp++;
        }
    }
    if (vf < 2) return wf;
    wf.validFolds = vf; wf.avgOosSharpe = tOos / vf;
    double avgIs = tIs / vf;
    wf.oosIsRatio = (std::abs(avgIs) > 1e-10) ? wf.avgOosSharpe / avgIs : 0;
    wf.passed = wf.oosIsRatio >= 0.6 && wf.avgOosSharpe > 0 && double(fp) / vf >= 0.6;
    return wf;
}

double whitesRealityCheck(const std::vector<double>& rets, int nBoot = 5000) {
    if (rets.size() < 30) return 1.0;
    size_t T = rets.size();
    double obs = std::accumulate(rets.begin(), rets.end(), 0.0) / T;
    if (obs <= 0) return 1.0;
    double avgBlock = std::max(2.0, std::sqrt(double(T))), pCont = 1.0 / avgBlock;
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> idxD(0, T-1);
    std::bernoulli_distribution contD(pCont);
    int exc = 0;
    for (int b = 0; b < nBoot; ++b) {
        double bm = 0; size_t cnt = 0; size_t pos = idxD(rng);
        while (cnt < T) { bm += rets[pos]; cnt++; if (!contD(rng)) pos = idxD(rng); else pos = (pos+1)%T; }
        bm /= double(cnt); if (bm >= obs) exc++;
    }
    return double(exc) / nBoot;
}

bool checkNoLookahead(const Factory& fac, SymbolId sid, const SymbolRegistry& reg,
                      const Params& p, const std::vector<BarEvent>& events, SizingMode sizing) {
    if (events.size() < 100) return true;
    size_t cut = events.size() * 80 / 100;
    auto fullR = runSingle(fac, sid, reg, p, events, sizing);
    auto truncR = runSingle(fac, sid, reg, p, {events.begin(), events.begin()+ptrdiff_t(cut)}, sizing);
    size_t est = fullR.trades * 80 / 100;
    return std::abs(int(est) - int(truncR.trades)) <= int(est * 0.15 + 1);
}

// ── Main ───────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    std::string data_dir = "data", tf = "4h", out_dir = "results", sym_filter = "";
    int max_sym = 0, years_back = 0, min_trades = 30, wf_folds = 5, wf_min_is = 5, wf_min_oos = 3;
    SizingMode sizingMode = SizingMode::FixedNotional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i+1 < argc) data_dir = argv[++i];
        else if (a == "--tf" && i+1 < argc) tf = argv[++i];
        else if (a == "--output" && i+1 < argc) out_dir = argv[++i];
        else if (a == "--max-symbols" && i+1 < argc) max_sym = std::stoi(argv[++i]);
        else if (a == "--years-back" && i+1 < argc) years_back = std::stoi(argv[++i]);
        else if (a == "--sizing" && i+1 < argc) { std::string m = argv[++i]; sizingMode = (m=="all_equity") ? SizingMode::AllEquity : SizingMode::FixedNotional; }
        else if (a == "--min-trades" && i+1 < argc) min_trades = std::stoi(argv[++i]);
        else if (a == "--wf-folds" && i+1 < argc) wf_folds = std::stoi(argv[++i]);
        else if (a == "--symbol" && i+1 < argc) sym_filter = argv[++i];
    }
    fs::create_directories(out_dir);
    auto csvs = findCsvFiles(data_dir, tf);

    auto strats = allStrats();
    std::vector<ExitMode> emodes = {ExitMode::Chandelier, ExitMode::ChandelierTimeStop, ExitMode::SignalBETrail};
    std::vector<GridConfig> grids = {
        {"donchian",        {10,15,20,25,30,40,50}, {0}, {0}},
        {"dual_momentum",   {10,15,20,30,40,50},    {0}, {0}},
        {"ema_crossover",   {5,8,10,12,15,20},       {20,30,40,50}, {0}},
        {"keltner_breakout",{10,15,20,25,30},         {0}, {1.5,2.0,2.5,3.0}},
        {"supertrend",      {7,10,14,21},             {0}, {2.0,2.5,3.0,3.5}},
        {"tsmom",           {10,15,20,30,40,50},      {15,20,30}, {0.15,0.20,0.30}},
        {"rsi2",            {2,3,4,5},                {5,10,20,50}, {5.0,10.0,15.0,20.0}},
        {"rsi_bb_mr",       {2,5,10,14},              {15,20,25}, {1.5,2.0,2.5}},
    };

    FILE* csvf = std::fopen((out_dir + "/grid_summary.csv").c_str(), "w");
    fmt::print(csvf, "symbol,strategy,exit_mode,p1,p2,p3,atr_per,sl_mult,trail_mult,sharpe,trades,return_pct,dd_pct,win_rate,plateau,neighbors,plateau_ratio,rel_variance,wf_sharpe,wf_oos_is_ratio,wf_passed,wrc_p,wrc_sig,no_lookahead\n");
    FILE* dumpf = std::fopen((out_dir + "/grid_all_combos.csv").c_str(), "w");
    fmt::print(dumpf, "symbol,strategy,exit_mode,p1,p2,p3,atr_per,sl_mult,trail_mult,sharpe,trades,return_pct,dd_pct,win_rate\n");

    SymbolRegistry registry;
    struct SymData { std::string name; SymbolId sid; std::vector<CsvBar> bars; std::vector<BarEvent> events; };
    std::vector<SymData> symData;
    for (const auto& csv : csvs) {
        std::string fn = fs::path(csv).stem().string();
        std::string sym = fn; auto pos = sym.find("USDT"); if (pos != std::string::npos) sym = sym.substr(0, pos + 4);
        if (!sym_filter.empty() && sym != sym_filter) continue;
        auto bars = loadCsv(csv);
        if (bars.empty()) continue;
        if (years_back > 0 && bars.size() > 1) {
            int64_t lastTs = bars.back().timestamp_ms;
            int64_t cutTs = lastTs - int64_t(years_back) * 365LL * 24 * 3600 * 1000;
            auto it = std::find_if(bars.begin(), bars.end(), [cutTs](const CsvBar& b) { return b.timestamp_ms >= cutTs; });
            bars.erase(bars.begin(), it);
        }
        if (bars.empty()) continue;
        SymbolInfo info; info.exchange = "binance"; info.symbol = sym; info.tickSize = Price::fromDouble(0.01);
        auto sid = registry.registerSymbol(info);
        auto events = csvToBarEvents(bars, sid);
        symData.push_back({sym, sid, std::move(bars), std::move(events)});
        fmt::print("[{}] {} bars\n", sym, symData.back().bars.size());
    }
    if (max_sym > 0 && int(symData.size()) > max_sym) symData.resize(max_sym);
    fmt::print("Symbols: {}\n\n", symData.size());

    int total_combos = 0, total_promoted = 0;
    for (const auto& sd : symData) {
        for (const auto& [sname, fac] : strats) {
            const GridConfig* gc = nullptr;
            for (const auto& g : grids) if (g.name == sname) { gc = &g; break; }
            if (!gc) continue;
            for (auto em : emodes) {
                std::vector<GridResult> results;
                for (int p1 : gc->p1) for (int p2 : gc->p2) for (double p3 : gc->p3)
                for (int ea : {10,14,21}) for (double sl : {0.8,1.0,1.2,1.5,2.0})
                for (double trail : {1.8,2.5,3.0,3.5,4.0}) {
                    total_combos++;
                    Params gp; gp.symbol = sd.name; gp.signal_p1 = p1;
                    gp.signal_p2 = p2 > 0 ? p2 : p1; gp.signal_p3 = p3 == 0 ? 2.0 : p3;
                    gp.exit_mode = em; gp.exit_atr_period = ea; gp.exit_sl_mult = sl;
                    gp.exit_trail_mult = trail;
                    gp.exit_time_bars = (em == ExitMode::ChandelierTimeStop) ? 40 : 0;
                    gp.exit_be_r_mult = 1.0;
                    auto rm = runSingle(fac, sd.sid, registry, gp, sd.events, sizingMode);
                    std::string ems = em == ExitMode::Chandelier ? "chan" : em == ExitMode::ChandelierTimeStop ? "chan_ts" : "sig_be";
                    if (std::isfinite(rm.sharpe))
                        fmt::print(dumpf, "{},{},{},{},{},{:.1f},{},{:.1f},{:.1f},{:.3f},{},{:.2f},{:.2f},{:.2f}\n",
                            sd.name, sname, ems, p1, gp.signal_p2, gp.signal_p3, ea, sl, trail, rm.sharpe, rm.trades, rm.totalReturn, rm.maxDrawdownPct, rm.winRate);
                    if (rm.trades >= size_t(min_trades) && std::isfinite(rm.sharpe)) {
                        results.push_back({gp, rm.sharpe, rm.trades, rm.totalReturn, rm.maxDrawdownPct, rm.winRate});
                    }
                }
                if (results.empty()) continue;
                std::sort(results.begin(), results.end(), [](auto& a, auto& b) { return a.sharpe > b.sharpe; });
                auto& bp = results[0].params; double bs = results[0].sharpe; size_t bt = results[0].trades;
                auto plateau = detectPlateau(results, 0, *gc);
                auto wf = walkForward(fac, sd.sid, registry, bp, sd.events, wf_folds, 0.7, 24, wf_min_is, wf_min_oos, sizingMode);
                auto bestRun = runSingle(fac, sd.sid, registry, bp, sd.events, sizingMode);
                double wrc_p = 1.0;
                if (bestRun.barReturns.size() >= 30 && bs > 0) wrc_p = whitesRealityCheck(bestRun.barReturns);
                bool noLA = checkNoLookahead(fac, sd.sid, registry, bp, sd.events, sizingMode);
                bool promoted = plateau.isPlateau && wf.passed && wrc_p < 0.05 && noLA && bs > 0;
                if (promoted) total_promoted++;
                std::string ems = em == ExitMode::Chandelier ? "chan" : em == ExitMode::ChandelierTimeStop ? "chan_ts" : "sig_be";
                fmt::print("  {} | {} | sh={:.2f} tr={} pl={}({} n={:.2f} v={:.2f}) wf={:.2f}(r={:.2f} f={} {}) wrc={:.4f} la={}\n",
                    sname, ems, bs, bt, plateau.isPlateau, plateau.neighborCount, plateau.neighborRatio, plateau.relVariance,
                    wf.avgOosSharpe, wf.oosIsRatio, wf.validFolds, wf.passed?"PASS":"FAIL", wrc_p, noLA?"OK":"SUSPECT");
                std::fprintf(csvf, "%s,%s,%s,%d,%d,%.1f,%d,%.1f,%.1f,%.3f,%lu,%.2f,%.2f,%.2f,%s,%d,%.3f,%.3f,%.3f,%.3f,%s,%d,%.4f,%s,%s\n",
                    sd.name.c_str(), sname.c_str(), ems.c_str(), bp.signal_p1, bp.signal_p2, bp.signal_p3, bp.exit_atr_period, bp.exit_sl_mult, bp.exit_trail_mult,
                    bs, bt, results[0].totalReturn, results[0].maxDrawdownPct, results[0].winRate,
                    plateau.isPlateau ? "1" : "0", plateau.neighborCount, plateau.neighborRatio, plateau.relVariance,
                    wf.avgOosSharpe, wf.oosIsRatio, wf.passed ? "1" : "0", wf.validFolds, wrc_p, wrc_p < 0.05 ? "1" : "0", noLA ? "1" : "0");
            }
        }
        std::fflush(csvf); std::fflush(dumpf);
        fmt::print("  [{}] done combos={} promoted={}\n", sd.name, total_combos, total_promoted);
    }
    std::fclose(csvf); std::fclose(dumpf);
    fmt::print("\nTotal: {} combos, {} promoted. Results: {}/grid_summary.csv\n", total_combos, total_promoted, out_dir);
    return 0;
}
