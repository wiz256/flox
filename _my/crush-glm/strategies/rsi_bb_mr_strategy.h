#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class RsiBbMrStrategy : public flox::Strategy {
public:
    struct Params {
        int rsi_window;
        double rsi_oversold;
        double rsi_overbought;
        int bb_window;
        double bb_std;
        std::string toString() const {
            return "rsi_w=" + std::to_string(rsi_window) + ",os=" + std::to_string(rsi_oversold)
                 + ",ob=" + std::to_string(rsi_overbought) + ",bb_w=" + std::to_string(bb_window)
                 + ",bb_s=" + std::to_string(bb_std);
        }
    };

    RsiBbMrStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
                    Params params, double size = 0.01)
        : Strategy(1, symbol, registry), params_(params),
          size_(flox::Quantity::fromDouble(size)) {}

    void start() override { running_ = true; }
    void stop() override { running_ = false; }

protected:
    void onSymbolTrade(flox::SymbolContext&, const flox::TradeEvent& ev) override {
        if (!running_) return;
        double price = ev.trade.price.toDouble();
        closes_.push_back(price);
        size_t need = static_cast<size_t>(std::max(params_.rsi_window, params_.bb_window) + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        double rsi_val = rsi(closes_, params_.rsi_window);
        auto bb = bollinger(closes_, params_.bb_window, params_.bb_std);
        if (std::isnan(rsi_val) || std::isnan(bb.upper)) return;

        double cur = closes_.back();

        if (!long_ && rsi_val < params_.rsi_oversold && cur < bb.lower) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (!short_ && rsi_val > params_.rsi_overbought && cur > bb.upper) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && cur >= bb.mid) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && cur <= bb.mid) {
            emitMarketBuy(symbol(), size_);
            short_ = false;
        }
    }

private:
    Params params_;
    flox::Quantity size_;
    std::deque<double> closes_;
    bool running_{false}, long_{false}, short_{false};
};

struct RsiBbMrGrid {
    std::vector<int> rsi_windows = {2, 5, 10, 14};
    std::vector<double> rsi_oversold_levels = {15.0, 20.0, 25.0, 30.0};
    std::vector<double> rsi_overbought_levels = {70.0, 75.0, 80.0, 85.0};
    std::vector<int> bb_windows = {15, 20, 25};
    std::vector<double> bb_stds = {1.5, 2.0, 2.5};
    size_t totalCombinations() const {
        return rsi_windows.size() * rsi_oversold_levels.size() * rsi_overbought_levels.size()
             * bb_windows.size() * bb_stds.size();
    }
    RsiBbMrStrategy::Params operator[](size_t i) const {
        size_t bs = i % bb_stds.size(); i /= bb_stds.size();
        size_t bw = i % bb_windows.size(); i /= bb_windows.size();
        size_t ob = i % rsi_overbought_levels.size(); i /= rsi_overbought_levels.size();
        size_t os = i % rsi_oversold_levels.size(); i /= rsi_oversold_levels.size();
        size_t rw = i % rsi_windows.size();
        return {rsi_windows[rw], rsi_oversold_levels[os], rsi_overbought_levels[ob],
                bb_windows[bw], bb_stds[bs]};
    }
};

} // namespace flox_my
