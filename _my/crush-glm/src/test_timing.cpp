#include "csv_reader.h"
#include "flox/aggregator/events/bar_event.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/indicator/atr.h"
#include "flox/strategy/strategy.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/rsi.h"
#include "flox/indicator/sma.h"
#include <fmt/format.h>
#include <chrono>

using namespace flox;
using namespace flox::indicator;
using namespace crush;

class SimpleDonchian : public Strategy {
    ATR atr_; Quantity qty_;
    std::vector<double> closes_, highs_, lows_;
public:
    SimpleDonchian(SubscriberId sid, SymbolId sym, const SymbolRegistry& reg)
        : Strategy(sid, sym, reg), atr_(14), qty_(Quantity::fromDouble(0.01)) {}
protected:
    void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override {
        double h = ev.bar.high.toDouble(), l = ev.bar.low.toDouble(), c = ev.bar.close.toDouble();
        closes_.push_back(c); highs_.push_back(h); lows_.push_back(l);
        atr_.update(h, l, c);
        if (closes_.size() < 21) return;
        double pu = *std::max_element(highs_.end()-21, highs_.end());
        double pl = *std::min_element(lows_.end()-21, lows_.end());
        Quantity pos = position(ev.symbol);
        if (pos.raw() == 0 && c > pu) emitMarketBuy(ev.symbol, qty_);
        else if (pos.raw() == 0 && c < pl) emitMarketSell(ev.symbol, qty_);
        else if (pos.raw() > 0 && c < pl) emitMarketSell(ev.symbol, qty_);
        else if (pos.raw() < 0 && c > pu) emitMarketBuy(ev.symbol, qty_);
    }
};

std::vector<BarEvent> makeBarEvents(const std::vector<CsvBar>& bars, SymbolId sym) {
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

int main() {
    using Clock = std::chrono::high_resolution_clock;

    auto csvBars = loadCsv("data/BTCUSDTUSDT_4h.csv");
    fmt::print("Loaded {} bars\n", csvBars.size());

    SymbolRegistry reg;
    SymbolInfo info;
    info.exchange = "binance"; info.symbol = "BTCUSDT"; info.tickSize = Price::fromDouble(0.01);
    auto sid = reg.registerSymbol(info);

    auto barEvents = makeBarEvents(csvBars, sid);
    fmt::print("Created {} BarEvents\n", barEvents.size());

    BacktestConfig cfg;
    cfg.initialCapital = 10000.0;
    cfg.feeRate = 0.0004;

    // Run 10 backtests and time each one
    for (int i = 0; i < 10; i++) {
        auto t0 = Clock::now();
        auto strat = std::make_unique<SimpleDonchian>(1, sid, reg);
        BacktestRunner runner(cfg);
        runner.setStrategy(strat.get());
        auto result = runner.runBars(barEvents);
        auto stats = result.computeStats();
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        fmt::print("  run {}: {:.1f}ms  trades={} sharpe={:.2f}\n", i, ms, stats.totalTrades, stats.sharpeRatio);
    }

    // Now run 100 to see if there's degradation
    fmt::print("\n100-run batch:\n");
    auto t0 = Clock::now();
    for (int i = 0; i < 100; i++) {
        auto strat = std::make_unique<SimpleDonchian>(1, sid, reg);
        BacktestRunner runner(cfg);
        runner.setStrategy(strat.get());
        auto result = runner.runBars(barEvents);
    }
    auto t1 = Clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    fmt::print("  100 runs: {:.0f}ms total, {:.1f}ms avg\n", totalMs, totalMs/100);

    return 0;
}
