#include "csv_reader.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/indicator/atr.h"
#include "flox/strategy/strategy.h"
#include "flox/replay/ohlcv_replay_source.h"
#include <fmt/format.h>
#include <iostream>

using namespace flox;
using namespace flox::indicator;
using namespace crush;

class TestStrat : public Strategy {
public:
    TestStrat(SymbolId sym, const SymbolRegistry& reg, const std::vector<CsvBar>* bars)
        : Strategy(1, sym, reg), bars_(bars), atr_(14),
          qty_(Quantity::fromDouble(0.01)) {}

    void start() override { running_ = true; }
    void stop() override { running_ = false; }

protected:
    void onSymbolTrade(SymbolContext& ctx, const TradeEvent& ev) override {
        if (!running_ || idx_ >= bars_->size()) return;
        const auto& b = (*bars_)[idx_++];
        double h = b.high, l = b.low, c = b.close;
        highs_.push_back(h); lows_.push_back(l); closes_.push_back(c);
        atr_.update(h, l, c);

        size_t w = 20;
        if (closes_.size() > w + 1) {
            double pu = *std::max_element(highs_.end() - ptrdiff_t(w) - 1, highs_.end() - 1);
            double pl = *std::min_element(lows_.end() - ptrdiff_t(w) - 1, lows_.end() - 1);

            if (!inPos_ && c > pu) {
                inPos_ = true; side_ = 1; entry_ = c;
                emitMarketBuy(symbol(), qty_);
                fmt::print("  BAR {} BUY@{:.2f} atr={:.4f}\n", idx_, c, atr_.ready()?atr_.value():0);
            } else if (!inPos_ && c < pl) {
                inPos_ = true; side_ = -1; entry_ = c;
                emitMarketSell(symbol(), qty_);
                fmt::print("  BAR {} SELL@{:.2f} atr={:.4f}\n", idx_, c, atr_.ready()?atr_.value():0);
            } else if (inPos_ && side_ == 1 && c < pl) {
                emitMarketSell(symbol(), qty_);
                fmt::print("  BAR {} EXIT_LONG@{:.2f} pnl={:.2f}\n", idx_, c, c - entry_);
                inPos_ = false;
            } else if (inPos_ && side_ == -1 && c > pu) {
                emitMarketBuy(symbol(), qty_);
                fmt::print("  BAR {} EXIT_SHORT@{:.2f} pnl={:.2f}\n", idx_, c, entry_ - c);
                inPos_ = false;
            }
        }
    }

    const std::vector<CsvBar>* bars_;
    ATR atr_;
    Quantity qty_;
    std::vector<double> closes_, highs_, lows_;
    size_t idx_ = 0;
    bool running_ = false, inPos_ = false;
    int side_ = 0;
    double entry_ = 0;
};

int main() {
    auto bars = loadCsv("data/BTCUSDTUSDT_4h.csv");
    if (bars.empty()) {
        fmt::print("No bars found\n");
        return 1;
    }
    fmt::print("Loaded {} bars\n", bars.size());

    // Use first 200 bars for quick test
    std::vector<CsvBar> testBars(bars.begin(), bars.begin() + std::min(size_t(200), bars.size()));

    SymbolRegistry registry;
    SymbolInfo info;
    info.exchange = "binance";
    info.symbol = "BTCUSDT";
    info.tickSize = Price::fromDouble(0.01);
    auto sid = registry.registerSymbol(info);
    fmt::print("Registered symbol, got SymbolId={}\n", sid);

    TestStrat strat(sid, registry, &testBars);
    fmt::print("Strategy created OK\n");

    BacktestConfig cfg;
    cfg.initialCapital = 10000.0;
    cfg.feeRate = 0.0004;
    BacktestRunner runner(cfg);
    runner.setStrategy(&strat);
    fmt::print("Runner configured, starting backtest...\n");

    std::vector<OhlcvReplaySource::Bar> obars;
    for (const auto& b : testBars) {
        obars.push_back({b.timestamp_ms * 1'000'000LL,
                         static_cast<int64_t>(b.close * 100'000'000.0), sid});
    }
    OhlcvReplaySource source(std::move(obars));
    fmt::print("Created {} replay bars\n", obars.size());

    auto result = runner.run(source);
    auto stats = result.computeStats();
    fmt::print("\nResult: trades={} sharpe={:.3f} return={:.1f}% maxdd={:.1f}%\n",
               stats.totalTrades, stats.sharpeRatio, stats.returnPct, stats.maxDrawdownPct);
    return 0;
}
