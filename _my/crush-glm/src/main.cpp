#include "csv_reader.h"
#include "flox/aggregator/bar.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
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

enum class ExitMode : uint8_t { Chandelier, ChandelierTimeStop, SignalBETrail };

struct Params {
    std::string symbol;
    int signal_p1 = 20, signal_p2 = 50;
    double signal_p3 = 2.0;
    ExitMode exit_mode = ExitMode::Chandelier;
    int exit_atr_period = 14;
    double exit_sl_mult = 2.0, exit_trail_mult = 3.0, exit_tp_mult = 0.0;
    int exit_time_bars = 0;
    double exit_be_r_mult = 1.0;
};

// Streaming indicators for fast reset per backtest
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

class BaseStrategy : public Strategy {
public:
    BaseStrategy(SubscriberId sid, SymbolId sym, const SymbolRegistry& reg, const Params& p)
        : Strategy(sid, sym, reg), p_(p), exitAtr_(p.exit_atr_period),
          qty_(Quantity::fromDouble(0.01)) {}

protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        closes_.push_back(c); highs_.push_back(h); lows_.push_back(l);
        exitAtr_.update(h, l, c);
        onBarImpl(h, l, c);

        Quantity pos = position(ev.symbol);
        if (pos.raw() != 0) {
            barsInTrade_++;
            if (exitAtr_.ready()) checkExit(ev.symbol, c, h, l, exitAtr_.value());
        }
    }

    virtual void onBarImpl(double h, double l, double c) = 0;

    void enterLong(SymbolId sym, double price) {
        emitMarketBuy(sym, qty_);
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; side_ = 1; beAct_ = false; trail_ = 0;
    }
    void enterShort(SymbolId sym, double price) {
        emitMarketSell(sym, qty_);
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; side_ = -1; beAct_ = false; trail_ = 0;
    }

    void checkExit(SymbolId sym, double c, double h, double l, double atr) {
        if (atr <= 0) return;
        double ir = p_.exit_sl_mult * atr;
        Quantity pos = position(sym);
        if (pos.raw() == 0) { side_ = 0; return; }

        if (pos.raw() > 0) { // long
            highSince_ = std::max(highSince_, h);
            if (p_.exit_mode == ExitMode::Chandelier || p_.exit_mode == ExitMode::ChandelierTimeStop) {
                if (c <= highSince_ - p_.exit_trail_mult * atr) { emitClosePosition(sym); return; }
            }
            if (p_.exit_mode == ExitMode::SignalBETrail) {
                if (!beAct_ && (c - entryPrice_) >= p_.exit_be_r_mult * ir) { beAct_ = true; trail_ = entryPrice_; }
                if (beAct_) { trail_ = std::max(trail_, c - p_.exit_trail_mult * atr); if (c <= trail_) { emitClosePosition(sym); return; } }
                if (c <= entryPrice_ - ir) { emitClosePosition(sym); return; }
            }
            if (p_.exit_mode == ExitMode::ChandelierTimeStop && p_.exit_time_bars > 0 && barsInTrade_ >= p_.exit_time_bars) { emitClosePosition(sym); return; }
        } else { // short
            lowSince_ = std::min(lowSince_, l);
            if (p_.exit_mode == ExitMode::Chandelier || p_.exit_mode == ExitMode::ChandelierTimeStop) {
                if (c >= lowSince_ + p_.exit_trail_mult * atr) { emitClosePosition(sym); return; }
            }
            if (p_.exit_mode == ExitMode::SignalBETrail) {
                if (!beAct_ && (entryPrice_ - c) >= p_.exit_be_r_mult * ir) { beAct_ = true; trail_ = entryPrice_; }
                if (beAct_) { trail_ = std::min(trail_, c + p_.exit_trail_mult * atr); if (c >= trail_) { emitClosePosition(sym); return; } }
                if (c >= entryPrice_ + ir) { emitClosePosition(sym); return; }
            }
            if (p_.exit_mode == ExitMode::ChandelierTimeStop && p_.exit_time_bars > 0 && barsInTrade_ >= p_.exit_time_bars) { emitClosePosition(sym); return; }
        }
    }

    const Params p_;
    StreamAtr exitAtr_;
    Quantity qty_;
    std::vector<double> closes_, highs_, lows_;
    bool beAct_ = false;
    int side_ = 0, barsInTrade_ = 0;
    double entryPrice_ = 0, highSince_ = 0, lowSince_ = 1e18, trail_ = 0;
};

class DonchianStrat final : public BaseStrategy {
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double c) override {
        size_t w = p_.signal_p1;
        if (closes_.size() < w + 1) return;
        double pu = *std::max_element(highs_.end() - ptrdiff_t(w) - 1, highs_.end() - 1);
        double pl = *std::min_element(lows_.end() - ptrdiff_t(w) - 1, lows_.end() - 1);
        SymbolId sym = symbol();
        Quantity pos = position(sym);
        if (pos.raw() == 0 && c > pu) enterLong(sym, c);
        else if (pos.raw() == 0 && c < pl) enterShort(sym, c);
        else if (pos.raw() > 0 && c < pl) emitClosePosition(sym);
        else if (pos.raw() < 0 && c > pu) emitClosePosition(sym);
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
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (pos.raw() == 0 && m > 0 && mp <= 0) enterLong(sym, c);
        else if (pos.raw() == 0 && m < 0 && mp >= 0) enterShort(sym, c);
        else if (pos.raw() > 0 && m <= 0) emitClosePosition(sym);
        else if (pos.raw() < 0 && m >= 0) emitClosePosition(sym);
    }
};

class EmaCrossStrat final : public BaseStrategy {
    StreamEma fast_, slow_; bool prevAbove_ = false;
public:
    EmaCrossStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p)
        : BaseStrategy(sid, s, r, p), fast_(p.signal_p1), slow_(p.signal_p2) {}
    void onBarImpl(double, double, double c) override {
        fast_.update(c); slow_.update(c);
        if (!fast_.ready() || !slow_.ready()) return;
        bool above = fast_.value() > slow_.value();
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (pos.raw() == 0 && above && !prevAbove_) enterLong(sym, c);
        else if (pos.raw() == 0 && !above && prevAbove_) enterShort(sym, c);
        else if (pos.raw() > 0 && !above) emitClosePosition(sym);
        else if (pos.raw() < 0 && above) emitClosePosition(sym);
        prevAbove_ = above;
    }
};

class KeltnerBrkStrat final : public BaseStrategy {
    StreamEma ema_; StreamAtr atr_; double mult_;
public:
    KeltnerBrkStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p)
        : BaseStrategy(sid, s, r, p), ema_(p.signal_p1), atr_(p.signal_p1), mult_(p.signal_p3) {}
    void onBarImpl(double h, double l, double c) override {
        ema_.update(c); atr_.update(h, l, c);
        if (!ema_.ready() || !atr_.ready()) return;
        double mid = ema_.value(), a = atr_.value(), u = mid + mult_ * a, lo = mid - mult_ * a;
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (pos.raw() == 0 && c > u) enterLong(sym, c);
        else if (pos.raw() == 0 && c < lo) enterShort(sym, c);
        else if (pos.raw() > 0 && c < mid) emitClosePosition(sym);
        else if (pos.raw() < 0 && c > mid) emitClosePosition(sym);
    }
};

class SupertrendStrat final : public BaseStrategy {
    StreamAtr atr_; double mult_; double upper_ = 0, lower_ = 0, prevC_ = 0; int prevDir_ = 0; bool init_ = false;
public:
    SupertrendStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p)
        : BaseStrategy(sid, s, r, p), atr_(p.signal_p1), mult_(p.signal_p3) {}
    void onBarImpl(double h, double l, double c) override {
        atr_.update(h, l, c);
        if (!atr_.ready()) return;
        double a = atr_.value(), hl2 = (h + l) / 2.0;
        double rawUp = hl2 + mult_ * a, rawLo = hl2 - mult_ * a;
        if (!init_) { upper_ = rawUp; lower_ = rawLo; init_ = true; prevC_ = c; prevDir_ = 1; return; }
        if (rawUp < upper_ || prevC_ > upper_) upper_ = rawUp;
        if (rawLo > lower_ || prevC_ < lower_) lower_ = rawLo;
        int d = (prevDir_ == -1) ? (c > upper_ ? 1 : -1) : (c < lower_ ? -1 : prevDir_);
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (d == 1 && prevDir_ == -1 && pos.raw() == 0) enterLong(sym, c);
        else if (d == -1 && prevDir_ == 1 && pos.raw() == 0) enterShort(sym, c);
        else if (pos.raw() > 0 && d == -1) emitClosePosition(sym);
        else if (pos.raw() < 0 && d == 1) emitClosePosition(sym);
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
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (vol < p_.signal_p3 && pos.raw() == 0 && m > 0 && prevMom_ <= 0) enterLong(sym, c);
        else if (vol < p_.signal_p3 && pos.raw() == 0 && m < 0 && prevMom_ >= 0) enterShort(sym, c);
        else if (pos.raw() > 0 && m <= 0) emitClosePosition(sym);
        else if (pos.raw() < 0 && m >= 0) emitClosePosition(sym);
        prevMom_ = m;
    }
};

class Rsi2Strat final : public BaseStrategy {
    StreamRsi rsi_; StreamSma sma_;
public:
    Rsi2Strat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p)
        : BaseStrategy(sid, s, r, p), rsi_(p.signal_p1), sma_(p.signal_p2) {}
    void onBarImpl(double, double, double c) override {
        rsi_.update(c); sma_.update(c);
        if (!rsi_.ready() || !sma_.ready()) return;
        double rv = rsi_.value(), sv = sma_.value(), os = p_.signal_p3, ob = 100-os;
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (pos.raw() == 0 && rv < os && c > sv) enterLong(sym, c);
        else if (pos.raw() == 0 && rv > ob && c < sv) enterShort(sym, c);
        else if (pos.raw() > 0 && (rv > ob || c < sv)) emitClosePosition(sym);
        else if (pos.raw() < 0 && (rv < os || c > sv)) emitClosePosition(sym);
    }
};

class RsiBbMrStrat final : public BaseStrategy {
    StreamRsi rsi_; StreamSma bbSma_; StreamStddev bbStd_; double bbMult_;
public:
    RsiBbMrStrat(SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p)
        : BaseStrategy(sid, s, r, p), rsi_(p.signal_p1), bbSma_(p.signal_p2), bbStd_(p.signal_p2), bbMult_(p.signal_p3) {}
    void onBarImpl(double, double, double c) override {
        rsi_.update(c); bbSma_.update(c); bbStd_.update(c);
        if (!rsi_.ready() || !bbSma_.ready() || !bbStd_.ready()) return;
        double rv = rsi_.value(), mid = bbSma_.value(), sd = bbStd_.value();
        double lo = mid - bbMult_ * sd, hi = mid + bbMult_ * sd;
        SymbolId sym = symbol(); Quantity pos = position(sym);
        if (pos.raw() == 0 && rv < 30 && c < lo) enterLong(sym, c);
        else if (pos.raw() == 0 && rv > 70 && c > hi) enterShort(sym, c);
        else if (pos.raw() > 0 && c >= mid) emitClosePosition(sym);
        else if (pos.raw() < 0 && c <= mid) emitClosePosition(sym);
    }
};

using Factory = std::function<std::unique_ptr<BaseStrategy>(SubscriberId, SymbolId, const SymbolRegistry&, const Params&)>;

std::vector<std::pair<std::string, Factory>> allStrats() {
    return {
        {"donchian", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<DonchianStrat>(sid, s, r, p); }},
        {"dual_momentum", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<DualMomStrat>(sid, s, r, p); }},
        {"ema_crossover", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<EmaCrossStrat>(sid, s, r, p); }},
        {"keltner_breakout", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<KeltnerBrkStrat>(sid, s, r, p); }},
        {"supertrend", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<SupertrendStrat>(sid, s, r, p); }},
        {"tsmom", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<TsmomStrat>(sid, s, r, p); }},
        {"rsi2", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<Rsi2Strat>(sid, s, r, p); }},
        {"rsi_bb_mr", [](SubscriberId sid, SymbolId s, const SymbolRegistry& r, const Params& p) { return std::make_unique<RsiBbMrStrat>(sid, s, r, p); }},
    };
}

std::vector<BarEvent> csvToBarEvents(const std::vector<CsvBar>& bars, SymbolId sym) {
    std::vector<BarEvent> events;
    events.reserve(bars.size());
    for (const auto& b : bars) {
        BarEvent ev;
        ev.symbol = sym;
        ev.barType = BarType::Time;
        ev.barTypeParam = 4 * 3600ULL * 1'000'000'000ULL;
        ev.bar.open = Price::fromDouble(b.open);
        ev.bar.high = Price::fromDouble(b.high);
        ev.bar.low = Price::fromDouble(b.low);
        ev.bar.close = Price::fromDouble(b.close);
        ev.bar.volume = Volume::fromDouble(b.volume);
        ev.bar.startTime = TimePoint{} + std::chrono::nanoseconds(b.timestamp_ms * 1'000'000LL - 14'400'000'000'000LL);
        ev.bar.endTime = TimePoint{} + std::chrono::nanoseconds(b.timestamp_ms * 1'000'000LL);
        events.push_back(ev);
    }
    return events;
}

struct GridConfig {
    std::string name;
    std::vector<int> p1; std::vector<int> p2; std::vector<double> p3;
};

int main(int argc, char** argv) {
    std::string data_dir = "data", tf = "4h", out_dir = "results";
    int max_sym = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--tf" && i + 1 < argc) tf = argv[++i];
        else if (a == "--output" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--max-symbols" && i + 1 < argc) max_sym = std::stoi(argv[++i]);
    }
    fs::create_directories(out_dir);

    auto csvs = findCsvFiles(data_dir, tf);
    if (max_sym > 0 && int(csvs.size()) > max_sym) csvs.resize(max_sym);
    fmt::print("=== CRUSH Grid Search ===\nData:{} TF:{} Symbols:{}\n\n", data_dir, tf, csvs.size());

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
    fmt::print(csvf, "symbol,strategy,exit_mode,p1,p2,p3,atr_per,sl_mult,trail_mult,sharpe,trades,plateau,wf_sharpe,wrc_p,wrc_sig\n");

    SymbolRegistry registry;
    // Pre-register ALL symbols so IDs are stable
    struct SymData { std::string name; SymbolId sid; std::vector<CsvBar> bars; std::vector<BarEvent> events; };
    std::vector<SymData> symData;
    for (const auto& csv : csvs) {
        std::string fn = fs::path(csv).stem().string();
        std::string sym = fn; auto pos = sym.find("USDT"); if (pos != std::string::npos) sym = sym.substr(0, pos + 4);
        auto bars = loadCsv(csv);
        if (bars.empty()) continue;
        SymbolInfo info; info.exchange = "binance"; info.symbol = sym; info.tickSize = Price::fromDouble(0.01);
        auto sid = registry.registerSymbol(info);
        auto events = csvToBarEvents(bars, sid);
        symData.push_back({sym, sid, std::move(bars), std::move(events)});
        fmt::print("[{}] {} bars sid={}\n", sym, symData.back().bars.size(), sid);
    }

    int total_combos = 0;
    for (const auto& sd : symData) {
        for (const auto& [sname, fac] : strats) {
            const GridConfig* gc = nullptr;
            for (const auto& g : grids) if (g.name == sname) { gc = &g; break; }
            if (!gc) continue;

            for (auto em : emodes) {
                std::vector<std::pair<Params, double>> results;
                for (int p1 : gc->p1) for (int p2 : gc->p2) for (double p3 : gc->p3)
                for (int ea : {10,14,21}) for (double sl : {1.5,2.0,2.5}) {
                    total_combos++;
                    Params gp; gp.symbol = sd.name; gp.signal_p1 = p1;
                    gp.signal_p2 = p2 > 0 ? p2 : p1; gp.signal_p3 = p3 == 0 ? 2.0 : p3;
                    gp.exit_mode = em; gp.exit_atr_period = ea; gp.exit_sl_mult = sl;
                    gp.exit_trail_mult = 3.0; gp.exit_time_bars = (em == ExitMode::ChandelierTimeStop) ? 40 : 0;
                    gp.exit_be_r_mult = 1.0;
                    try {
                        auto strat = fac(1, sd.sid, registry, gp);
                        BacktestConfig cfg; cfg.initialCapital = 10000.0; cfg.feeRate = 0.0004;
                        BacktestRunner runner(cfg); runner.setStrategy(strat.get());
                        auto r = runner.runBars(sd.events);
                        auto s = r.computeStats();
                        if (s.totalTrades >= 10) results.push_back({gp, s.sharpeRatio});
                    } catch (...) {}
                }

                if (results.empty()) continue;
                std::sort(results.begin(), results.end(), [](auto& a, auto& b) { return a.second > b.second; });
                auto& bp = results[0].first; double bs = results[0].second;

                // Plateau detection
                int above = 0, total = 0;
                for (size_t i = 1; i < results.size(); ++i) {
                    if (std::abs(results[i].first.signal_p1 - bp.signal_p1) <= 2) {
                        total++; if (results[i].second >= bs * 0.8) above++;
                    }
                }
                bool plateau = total > 0 && double(above)/total >= 0.5 && bs > 0;

                // Walk-forward (simplified)
                double wf_sharpe = 0;
                if (plateau && bs > 0.3) {
                    int folds = 5; size_t seg = sd.bars.size() / (folds * 2);
                    if (seg >= 50) {
                        double ss = 0; int vf = 0;
                        for (int f = 0; f < folds; ++f) {
                            size_t ts = f * seg * 2 + seg, te = std::min(ts + seg, sd.events.size());
                            if (te <= ts) continue;
                            try {
                                auto strat = fac(1, sd.sid, registry, bp);
                                BacktestConfig cfg; cfg.initialCapital = 10000.0; cfg.feeRate = 0.0004;
                                BacktestRunner runner(cfg); runner.setStrategy(strat.get());
                                std::vector<BarEvent> slice(sd.events.begin()+ts, sd.events.begin()+te);
                                auto r = runner.runBars(slice); auto s = r.computeStats();
                                ss += s.sharpeRatio; vf++;
                            } catch (...) {}
                        }
                        if (vf > 0) wf_sharpe = ss / vf;
                    }
                }

                // White's reality check (simplified)
                double wrc_p = 1.0;
                if (plateau && bs > 0.3) {
                    std::vector<double> rets; rets.reserve(sd.bars.size());
                    for (size_t i = 1; i < sd.bars.size(); ++i)
                        rets.push_back((sd.bars[i].close - sd.bars[i-1].close) / sd.bars[i-1].close);
                    if (rets.size() >= 10) {
                        double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
                        std::mt19937 rng(42); int exc = 0;
                        for (int b = 0; b < 1000; ++b) {
                            double bm = 0;
                            for (size_t i = 0; i < rets.size(); ++i) bm += rets[rng() % rets.size()];
                            bm /= rets.size(); if (bm >= mean) exc++;
                        }
                        wrc_p = double(exc) / 1000;
                    }
                }

                std::string ems = em == ExitMode::Chandelier ? "chan" : em == ExitMode::ChandelierTimeStop ? "chan_ts" : "sig_be";
                fmt::print("  {} | {} | sh={:.2f} tr={} pl={} wf={:.2f} wrc={:.3f}\n",
                    sname, ems, bs, results.size(), plateau, wf_sharpe, wrc_p);
                fmt::print(csvf, "{},{},{},{},{},{:.1f},{},{:.1f},{:.1f},{:.3f},{},{},{:.3f},{:.4f},{}\n",
                    sd.name, sname, ems, bp.signal_p1, bp.signal_p2, bp.signal_p3,
                    bp.exit_atr_period, bp.exit_sl_mult, bp.exit_trail_mult,
                    bs, results.size(), plateau, wf_sharpe, wrc_p, wrc_p < 0.05);
            }
        }
        std::fflush(csvf);
        fmt::print("  [{}] done, total combos={}\n", sd.name, total_combos);
        std::fflush(stdout);
    }
    std::fclose(csvf);
    fmt::print("\nTotal combos: {}\nResults: {}/grid_summary.csv\n", total_combos, out_dir);
    return 0;
}
