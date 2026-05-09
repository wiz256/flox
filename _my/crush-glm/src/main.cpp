#include "csv_reader.h"

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/bollinger.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/keltner.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/sma.h"
#include "flox/indicator/supertrend.h"
#include "flox/strategy/strategy.h"
#include "flox/replay/ohlcv_replay_source.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fmt/format.h>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace flox;
using namespace flox::indicator;
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
    std::string toString() const {
        std::string em = exit_mode == ExitMode::Chandelier ? "chan"
                       : exit_mode == ExitMode::ChandelierTimeStop ? "chan_ts" : "sig_be";
        return fmt::format("{}|p1={}|p2={}|p3={:.1f}|{}|eatr={}|sl={:.1f}|tr={:.1f}|tb={}",
            symbol.substr(0,8), signal_p1, signal_p2, signal_p3, em,
            exit_atr_period, exit_sl_mult, exit_trail_mult, exit_time_bars);
    }
};

class BaseStrategy : public Strategy {
public:
    BaseStrategy(SymbolId sym, const SymbolRegistry& reg, const Params& p,
                 const std::vector<CsvBar>* csvBars)
        : Strategy(1, sym, reg), p_(p), atr_(p.exit_atr_period),
          csvBars_(csvBars), qty_(Quantity::fromDouble(0.01)) {}

    void start() override { running_ = true; }
    void stop() override { running_ = false; }

protected:
    void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override {
        if (!running_) return;
        if (barIdx_ >= csvBars_->size()) return;

        const CsvBar& bar = (*csvBars_)[barIdx_];
        double o = bar.open, h = bar.high, l = bar.low, c = bar.close;
        barIdx_++;
        closes_.push_back(c);
        highs_.push_back(h);
        lows_.push_back(l);
        atr_.update(h, l, c);

        onBarImpl(o, h, l, c);

        if (inPos_) {
            barsInTrade_++;
            checkExit(c, h, l, atr_.ready() ? atr_.value() : 0);
        }
    }

    virtual void onBarImpl(double open, double high, double low, double close) = 0;

    void enterLong(double price) {
        if (inPos_) return;
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; inPos_ = true; side_ = 1; beAct_ = false; trail_ = 0;
        emitMarketBuy(symbol(), qty_);
    }
    void enterShort(double price) {
        if (inPos_) return;
        entryPrice_ = price; highSince_ = price; lowSince_ = price;
        barsInTrade_ = 0; inPos_ = true; side_ = -1; beAct_ = false; trail_ = 0;
        emitMarketSell(symbol(), qty_);
    }
    void exitPos() {
        if (!inPos_) return;
        if (side_ == 1) emitMarketSell(symbol(), qty_);
        else emitMarketBuy(symbol(), qty_);
        inPos_ = false;
    }

    void checkExit(double c, double h, double l, double atr) {
        if (atr <= 0) return;
        double initialRisk = p_.exit_sl_mult * atr;

        if (side_ == 1) {
            highSince_ = std::max(highSince_, h);

            if (p_.exit_mode == ExitMode::Chandelier ||
                p_.exit_mode == ExitMode::ChandelierTimeStop) {
                if (c <= highSince_ - p_.exit_trail_mult * atr) { exitPos(); return; }
            }

            if (p_.exit_mode == ExitMode::SignalBETrail) {
                if (!beAct_ && (c - entryPrice_) >= p_.exit_be_r_mult * initialRisk) {
                    beAct_ = true;
                    trail_ = entryPrice_;
                }
                if (beAct_) {
                    trail_ = std::max(trail_, c - p_.exit_trail_mult * atr);
                    if (c <= trail_) { exitPos(); return; }
                }
                if (c <= entryPrice_ - initialRisk) { exitPos(); return; }
                if (p_.exit_tp_mult > 0 && c >= entryPrice_ + p_.exit_tp_mult * atr) { exitPos(); return; }
            }

            if (p_.exit_mode == ExitMode::ChandelierTimeStop &&
                p_.exit_time_bars > 0 && barsInTrade_ >= p_.exit_time_bars) {
                exitPos(); return;
            }
        } else if (side_ == -1) {
            lowSince_ = std::min(lowSince_, l);

            if (p_.exit_mode == ExitMode::Chandelier ||
                p_.exit_mode == ExitMode::ChandelierTimeStop) {
                if (c >= lowSince_ + p_.exit_trail_mult * atr) { exitPos(); return; }
            }

            if (p_.exit_mode == ExitMode::SignalBETrail) {
                if (!beAct_ && (entryPrice_ - c) >= p_.exit_be_r_mult * initialRisk) {
                    beAct_ = true;
                    trail_ = entryPrice_;
                }
                if (beAct_) {
                    trail_ = std::min(trail_, c + p_.exit_trail_mult * atr);
                    if (c >= trail_) { exitPos(); return; }
                }
                if (c >= entryPrice_ + initialRisk) { exitPos(); return; }
                if (p_.exit_tp_mult > 0 && c <= entryPrice_ - p_.exit_tp_mult * atr) { exitPos(); return; }
            }

            if (p_.exit_mode == ExitMode::ChandelierTimeStop &&
                p_.exit_time_bars > 0 && barsInTrade_ >= p_.exit_time_bars) {
                exitPos(); return;
            }
        }
    }

    const Params p_;
    ATR atr_;
    const std::vector<CsvBar>* csvBars_;
    Quantity qty_;
    std::vector<double> closes_, highs_, lows_;
    bool running_ = false, inPos_ = false, beAct_ = false;
    int side_ = 0, barsInTrade_ = 0;
    size_t barIdx_ = 0;
    double entryPrice_ = 0, highSince_ = 0, lowSince_ = 1e18, trail_ = 0;
};

class DonchianStrat final : public BaseStrategy {
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double, double c) override {
        size_t w = static_cast<size_t>(p_.signal_p1);
        if (closes_.size() < w + 1) return;
        double pu = *std::max_element(highs_.end() - static_cast<ptrdiff_t>(w) - 1, highs_.end() - 1);
        double pl = *std::min_element(lows_.end() - static_cast<ptrdiff_t>(w) - 1, lows_.end() - 1);
        if (!inPos_ && c > pu) enterLong(c);
        else if (!inPos_ && c < pl) enterShort(c);
        else if (inPos_ && side_ == 1 && c < pl) exitPos();
        else if (inPos_ && side_ == -1 && c > pu) exitPos();
    }
};

class DualMomStrat final : public BaseStrategy {
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double, double c) override {
        int lb = p_.signal_p1;
        if (static_cast<int>(closes_.size()) < lb + 2) return;
        double m = (closes_.back() - closes_[closes_.size() - 1 - lb]) / closes_[closes_.size() - 1 - lb];
        double mp = (closes_[closes_.size() - 2] - closes_[closes_.size() - 2 - lb]) / closes_[closes_.size() - 2 - lb];
        if (!inPos_ && m > 0 && mp <= 0) enterLong(c);
        else if (!inPos_ && m < 0 && mp >= 0) enterShort(c);
        else if (inPos_ && side_ == 1 && m <= 0) exitPos();
        else if (inPos_ && side_ == -1 && m >= 0) exitPos();
    }
};

class EmaCrossStrat final : public BaseStrategy {
    EMA fast_, slow_;
    bool prevAbove_ = false;
public:
    EmaCrossStrat(SymbolId s, const SymbolRegistry& r, const Params& p,
                  const std::vector<CsvBar>* bars)
        : BaseStrategy(s, r, p, bars), fast_(p.signal_p1), slow_(p.signal_p2) {}
    void onBarImpl(double, double, double, double c) override {
        fast_.update(c); slow_.update(c);
        if (!fast_.ready() || !slow_.ready()) return;
        bool above = fast_.value() > slow_.value();
        if (!inPos_ && above && !prevAbove_) enterLong(c);
        else if (!inPos_ && !above && prevAbove_) enterShort(c);
        else if (inPos_ && side_ == 1 && !above) exitPos();
        else if (inPos_ && side_ == -1 && above) exitPos();
        prevAbove_ = above;
    }
};

class KeltnerBrkStrat final : public BaseStrategy {
    Keltner kc_;
public:
    KeltnerBrkStrat(SymbolId s, const SymbolRegistry& r, const Params& p,
                    const std::vector<CsvBar>* bars)
        : BaseStrategy(s, r, p, bars), kc_(p.signal_p1, p.signal_p1, p.signal_p3) {}
    void onBarImpl(double, double h, double l, double c) override {
        kc_.update(h, l, c);
        if (!kc_.ready()) return;
        double u = kc_.upperValue(), m = kc_.middleValue(), lo = kc_.lowerValue();
        if (std::isnan(u)) return;
        if (!inPos_ && c > u) enterLong(c);
        else if (!inPos_ && c < lo) enterShort(c);
        else if (inPos_ && side_ == 1 && c < m) exitPos();
        else if (inPos_ && side_ == -1 && c > m) exitPos();
    }
};

class SupertrendStrat final : public BaseStrategy {
    Supertrend st_;
    int prevDir_ = 0;
public:
    SupertrendStrat(SymbolId s, const SymbolRegistry& r, const Params& p,
                    const std::vector<CsvBar>* bars)
        : BaseStrategy(s, r, p, bars), st_(p.signal_p1, p.signal_p3) {}
    void onBarImpl(double, double h, double l, double c) override {
        st_.update(h, l, c);
        if (!st_.ready()) return;
        auto r = st_.computeFull(
            std::span<const double>(highs_),
            std::span<const double>(lows_),
            std::span<const double>(closes_));
        int d = r.direction.empty() ? 0 : r.direction.back();
        if (d == 1 && prevDir_ == -1 && !inPos_) enterLong(c);
        else if (d == -1 && prevDir_ == 1 && !inPos_) enterShort(c);
        else if (inPos_ && side_ == 1 && d == -1) exitPos();
        else if (inPos_ && side_ == -1 && d == 1) exitPos();
        if (d != 0) prevDir_ = d;
    }
};

class TsmomStrat final : public BaseStrategy {
    double prevMom_ = 0;
public:
    using BaseStrategy::BaseStrategy;
    void onBarImpl(double, double, double, double c) override {
        int lb = p_.signal_p1;
        size_t vw = p_.signal_p2 > 0 ? static_cast<size_t>(p_.signal_p2) : 20;
        if (closes_.size() < std::max(static_cast<size_t>(lb), vw) + 2) return;
        double m = (closes_.back() - closes_[closes_.size() - 1 - lb]) / closes_[closes_.size() - 1 - lb];
        std::vector<double> rets;
        rets.reserve(vw);
        for (size_t i = closes_.size() - vw; i < closes_.size(); ++i)
            rets.push_back(closes_[i] / closes_[i - 1] - 1.0);
        double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
        double sq = 0;
        for (double r : rets) sq += (r - mean) * (r - mean);
        double vol = std::sqrt(sq / rets.size()) * std::sqrt(252.0);
        if (vol < p_.signal_p3 && !inPos_ && m > 0 && prevMom_ <= 0) enterLong(c);
        else if (vol < p_.signal_p3 && !inPos_ && m < 0 && prevMom_ >= 0) enterShort(c);
        else if (inPos_ && side_ == 1 && m <= 0) exitPos();
        else if (inPos_ && side_ == -1 && m >= 0) exitPos();
        prevMom_ = m;
    }
};

class Rsi2Strat final : public BaseStrategy {
    RSI rsi_;
    SMA sma_;
public:
    Rsi2Strat(SymbolId s, const SymbolRegistry& r, const Params& p,
              const std::vector<CsvBar>* bars)
        : BaseStrategy(s, r, p, bars), rsi_(p.signal_p1), sma_(p.signal_p2) {}
    void onBarImpl(double, double, double, double c) override {
        rsi_.update(c); sma_.update(c);
        if (!rsi_.ready() || !sma_.ready()) return;
        double rv = rsi_.value(), sv = sma_.value();
        double os = p_.signal_p3, ob = 100.0 - os;
        bool up = c > sv;
        if (!inPos_ && rv < os && up) enterLong(c);
        else if (!inPos_ && rv > ob && !up) enterShort(c);
        else if (inPos_ && side_ == 1 && (rv > ob || c < sv)) exitPos();
        else if (inPos_ && side_ == -1 && (rv < os || c > sv)) exitPos();
    }
};

class RsiBbMrStrat final : public BaseStrategy {
    RSI rsi_;
    Bollinger bb_;
public:
    RsiBbMrStrat(SymbolId s, const SymbolRegistry& r, const Params& p,
                 const std::vector<CsvBar>* bars)
        : BaseStrategy(s, r, p, bars), rsi_(p.signal_p1), bb_(p.signal_p2, p.signal_p3) {}
    void onBarImpl(double, double, double, double c) override {
        rsi_.update(c); bb_.update(c);
        if (!rsi_.ready() || !bb_.ready()) return;
        double rv = rsi_.value(), lo = bb_.lowerValue(), hi = bb_.upperValue(), mid = bb_.middleValue();
        if (std::isnan(lo) || std::isnan(hi)) return;
        if (!inPos_ && rv < 30 && c < lo) enterLong(c);
        else if (!inPos_ && rv > 70 && c > hi) enterShort(c);
        else if (inPos_ && side_ == 1 && c >= mid) exitPos();
        else if (inPos_ && side_ == -1 && c <= mid) exitPos();
    }
};

using Factory = std::function<std::unique_ptr<BaseStrategy>(
    SymbolId, const SymbolRegistry&, const Params&, const std::vector<CsvBar>*)>;

struct StratDef { std::string name; Factory factory; };

std::vector<StratDef> allStrats() {
    return {
        {"donchian",        [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<DonchianStrat>(s, r, p, b); }},
        {"dual_momentum",   [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<DualMomStrat>(s, r, p, b); }},
        {"ema_crossover",   [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<EmaCrossStrat>(s, r, p, b); }},
        {"keltner_breakout",[](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<KeltnerBrkStrat>(s, r, p, b); }},
        {"supertrend",      [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<SupertrendStrat>(s, r, p, b); }},
        {"tsmom",           [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<TsmomStrat>(s, r, p, b); }},
        {"rsi2",            [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<Rsi2Strat>(s, r, p, b); }},
        {"rsi_bb_mr",       [](SymbolId s, const SymbolRegistry& r, const Params& p, const std::vector<CsvBar>* b)
                             { return std::make_unique<RsiBbMrStrat>(s, r, p, b); }},
    };
}

BacktestResult runBt(const std::vector<CsvBar>& bars, SymbolId sym, const SymbolRegistry& reg,
                     const Factory& fac, const Params& p) {
    auto strat = fac(sym, reg, p, &bars);
    BacktestConfig cfg;
    cfg.initialCapital = 10000.0;
    cfg.feeRate = 0.0004;
    BacktestRunner runner(cfg);
    runner.setStrategy(strat.get());

    std::vector<OhlcvReplaySource::Bar> obars;
    obars.reserve(bars.size());
    for (const auto& b : bars) {
        obars.push_back({b.timestamp_ms * 1'000'000LL,
                         static_cast<int64_t>(b.close * 100'000'000.0), sym});
    }
    OhlcvReplaySource source(std::move(obars));

    return runner.run(source);
}

struct WFResult { double avg_sharpe = 0; std::string report; };

WFResult walkForward(const std::vector<CsvBar>& bars, SymbolId sym, const SymbolRegistry& reg,
                     const Factory& fac, const Params& bp, int folds = 5) {
    WFResult w;
    size_t segSize = bars.size() / (folds * 2);
    if (segSize < 50) { w.report = "too_few_bars"; return w; }

    double sharpeSum = 0;
    int validFolds = 0;
    for (int f = 0; f < folds; ++f) {
        size_t trainStart = f * segSize * 2;
        size_t testStart = trainStart + segSize;
        size_t testEnd = std::min(testStart + segSize, bars.size());
        if (testEnd <= testStart) continue;

        std::vector<CsvBar> testSlice(bars.begin() + testStart, bars.begin() + testEnd);
        auto r = runBt(testSlice, sym, reg, fac, bp);
        auto s = r.computeStats();
        sharpeSum += s.sharpeRatio;
        validFolds++;
    }
    if (validFolds > 0) w.avg_sharpe = sharpeSum / validFolds;
    w.report = fmt::format("WF({}): avg_oos_sharpe={:.3f}", validFolds, w.avg_sharpe);
    return w;
}

struct WRCResult { double p = 1; bool sig = false; std::string report; };

WRCResult whitesRC(const std::vector<double>& rets, int boot = 1000) {
    WRCResult w;
    if (rets.empty() || rets.size() < 10) { w.report = "no_data"; return w; }
    double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
    std::mt19937 rng(42);
    int exceed = 0;
    for (int b = 0; b < boot; ++b) {
        double bootMean = 0;
        for (size_t i = 0; i < rets.size(); ++i)
            bootMean += rets[rng() % rets.size()];
        bootMean /= rets.size();
        if (bootMean >= mean) exceed++;
    }
    w.p = static_cast<double>(exceed) / boot;
    w.sig = w.p < 0.05;
    w.report = fmt::format("WRC: p={:.4f} sig={}", w.p, w.sig ? "YES" : "no");
    return w;
}

struct Plateau { bool found = false; double ratio = 0; std::string report; };

Plateau detectPlateau(const std::vector<std::pair<Params, double>>& res, size_t bi) {
    Plateau pl;
    if (res.empty()) { pl.report = "no_data"; return pl; }
    double threshold = res[bi].second * 0.8;
    int above = 0, total = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        if (i == bi) continue;
        bool neighbor = std::abs(res[i].first.signal_p1 - res[bi].first.signal_p1) <= 2;
        if (neighbor) {
            total++;
            if (res[i].second >= threshold) above++;
        }
    }
    pl.ratio = total > 0 ? static_cast<double>(above) / total : 0;
    pl.found = pl.ratio >= 0.5 && res[bi].second > 0;
    pl.report = fmt::format("plateau: ratio={:.2f} found={}", pl.ratio, pl.found ? "YES" : "no");
    return pl;
}

int main(int argc, char** argv) {
    std::string data_dir = "data", tf = "4h", out_dir = "results";
    int max_sym = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--tf" && i + 1 < argc) tf = argv[++i];
        else if (a == "--output" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--max-symbols" && i + 1 < argc) max_sym = std::stoi(argv[++i]);
        else if (a == "--help") {
            fmt::print("Usage: crush_grid [--data-dir DIR] [--tf TF] [--output DIR] [--max-symbols N]\n");
            return 0;
        }
    }
    fs::create_directories(out_dir);

    auto csvs = findCsvFiles(data_dir, tf);
    if (max_sym > 0 && static_cast<int>(csvs.size()) > max_sym) csvs.resize(max_sym);

    fmt::print("=== CRUSH Grid Search ===\nData:{} TF:{} Symbols:{} Out:{}\n\n",
               data_dir, tf, csvs.size(), out_dir);

    SymbolRegistry registry;
    auto strats = allStrats();
    std::vector<ExitMode> emodes = {
        ExitMode::Chandelier,
        ExitMode::ChandelierTimeStop,
        ExitMode::SignalBETrail
    };

    struct GridConfig {
        std::string name;
        std::vector<int> p1;
        std::vector<int> p2;
        std::vector<double> p3;
    };
    std::vector<GridConfig> grids = {
        {"donchian",        {10, 15, 20, 25, 30, 40, 50},       {0},              {0}},
        {"dual_momentum",   {10, 15, 20, 30, 40, 50},          {0},              {0}},
        {"ema_crossover",   {5, 8, 10, 12, 15, 20},             {20, 30, 40, 50}, {0}},
        {"keltner_breakout",{10, 15, 20, 25, 30},               {0},              {1.5, 2.0, 2.5, 3.0}},
        {"supertrend",      {7, 10, 14, 21},                    {0},              {2.0, 2.5, 3.0, 3.5}},
        {"tsmom",           {10, 15, 20, 30, 40, 50},           {15, 20, 30},     {0.15, 0.20, 0.30}},
        {"rsi2",            {2, 3, 4, 5},                       {5, 10, 20, 50},  {5.0, 10.0, 15.0, 20.0}},
        {"rsi_bb_mr",       {2, 5, 10, 14},                     {15, 20, 25},     {1.5, 2.0, 2.5}},
    };

    std::string csv_path = out_dir + "/grid_summary.csv";
    FILE* csvf = std::fopen(csv_path.c_str(), "w");
    if (!csvf) {
        fmt::print("ERROR: Cannot open {}\n", csv_path);
        return 1;
    }
    fmt::print(csvf, "symbol,strategy,exit_mode,p1,p2,p3,atr_per,sl_mult,trail_mult,"
                     "sharpe,return_pct,max_dd,trades,plateau,wf_sharpe,wrc_p,wrc_sig\n");

    int total_combos = 0, total_with_trades = 0;

    for (const auto& csvFile : csvs) {
        std::string fn = fs::path(csvFile).stem().string();
        std::string sym = fn;
        auto pos = sym.find("USDT");
        if (pos != std::string::npos) sym = sym.substr(0, pos + 4);

        auto bars = loadCsv(csvFile);
        if (bars.empty()) { fmt::print("[{}] no bars\n", sym); continue; }
        fmt::print("[{}] {} bars\n", sym, bars.size());

        SymbolInfo symInfo;
        symInfo.exchange = "binance";
        symInfo.symbol = sym;
        symInfo.tickSize = Price::fromDouble(0.01);
        auto sid = registry.registerSymbol(symInfo);

        for (const auto& sd : strats) {
            const GridConfig* gc = nullptr;
            for (const auto& g : grids)
                if (g.name == sd.name) { gc = &g; break; }
            if (!gc) continue;

            for (auto em : emodes) {
                std::vector<std::pair<Params, double>> results;

                for (int p1 : gc->p1)
                for (int p2 : gc->p2)
                for (double p3 : gc->p3)
                for (int ea : {10, 14, 21})
                for (double sl : {1.5, 2.0, 2.5}) {
                    total_combos++;
                    Params gp;
                    gp.symbol = sym;
                    gp.signal_p1 = p1;
                    gp.signal_p2 = p2 > 0 ? p2 : p1;
                    gp.signal_p3 = p3 == 0 ? 2.0 : p3;
                    gp.exit_mode = em;
                    gp.exit_atr_period = ea;
                    gp.exit_sl_mult = sl;
                    gp.exit_trail_mult = 3.0;
                    gp.exit_time_bars = (em == ExitMode::ChandelierTimeStop) ? 40 : 0;
                    gp.exit_be_r_mult = 1.0;

                    try {
                        auto r = runBt(bars, sid, registry, sd.factory, gp);
                        auto s = r.computeStats();
                        if (s.totalTrades >= 10) {
                            results.push_back({gp, s.sharpeRatio});
                            total_with_trades++;
                        }
                    } catch (const std::exception& e) {
                        fmt::print("    ERR: {} params={}\n", e.what(), gp.toString());
                    } catch (...) {
                        fmt::print("    ERR: unknown params={}\n", gp.toString());
                    }
                }

                if (results.empty()) continue;

                std::sort(results.begin(), results.end(),
                          [](auto& a, auto& b) { return a.second > b.second; });
                auto pl = detectPlateau(results, 0);
                auto& bp = results[0].first;
                double bestSharpe = results[0].second;

                WFResult wf;
                WRCResult wrc;
                if (pl.found && bestSharpe > 0.3) {
                    wf = walkForward(bars, sid, registry, sd.factory, bp, 5);
                    auto wr = runBt(bars, sid, registry, sd.factory, bp);
                    std::vector<double> barReturns;
                    barReturns.reserve(bars.size());
                    for (size_t i = 1; i < bars.size(); ++i)
                        barReturns.push_back((bars[i].close - bars[i-1].close) / bars[i-1].close);
                    wrc = whitesRC(barReturns, 1000);
                }

                std::string ems = em == ExitMode::Chandelier ? "chan"
                                : em == ExitMode::ChandelierTimeStop ? "chan_ts" : "sig_be";
                fmt::print("  {} | {} | sharpe={:.2f} trades={} | {} | {} | {}\n",
                    sd.name, ems, bestSharpe, results.size(),
                    pl.report, wf.report, wrc.report);

                fmt::print(csvf, "{},{},{},{},{},{:.1f},{},{:.1f},{:.1f},{:.3f},{:.1f},{:.1f},{},{},{:.3f},{:.4f},{}\n",
                    sym, sd.name, ems, bp.signal_p1, bp.signal_p2, bp.signal_p3,
                    bp.exit_atr_period, bp.exit_sl_mult, bp.exit_trail_mult,
                    bestSharpe, 0.0, 0.0, results.size(),
                    pl.found, wf.avg_sharpe, wrc.p, wrc.sig);
            }
        }
    }
    std::fclose(csvf);
    fmt::print("\nCombos tested: {} | With >=10 trades: {}\n", total_combos, total_with_trades);
    fmt::print("Results: {}\n", csv_path);
    return 0;
}
