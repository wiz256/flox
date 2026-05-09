#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class EmaCrossoverStrategy : public flox::Strategy {
public:
    struct Params {
        int fast;
        int slow;
        double adx_threshold;
        int adx_window;
        std::string toString() const {
            return "f=" + std::to_string(fast) + ",s=" + std::to_string(slow)
                 + ",adx_th=" + std::to_string(adx_threshold) + ",adx_w=" + std::to_string(adx_window);
        }
    };

    EmaCrossoverStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        size_t need = static_cast<size_t>(std::max(params_.slow, params_.adx_window) + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        double fast_ema = ema(closes_, params_.fast);
        double slow_ema = ema(closes_, params_.slow);
        if (std::isnan(fast_ema) || std::isnan(slow_ema)) return;

        bool above = fast_ema > slow_ema;

        if (params_.adx_threshold > 0) {
            double adx_val = adx(closes_, params_.adx_window);
            if (std::isnan(adx_val) || adx_val < params_.adx_threshold) {
                prev_above_ = above;
                return;
            }
        }

        if (above && !prev_above_ && !long_) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (!above && prev_above_ && !short_) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && !above) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && above) {
            emitMarketBuy(symbol(), size_);
            short_ = false;
        }
        prev_above_ = above;
    }

private:
    Params params_;
    flox::Quantity size_;
    std::deque<double> closes_;
    bool running_{false}, prev_above_{false}, long_{false}, short_{false};
};

struct EmaCrossoverGrid {
    std::vector<int> fast_periods = {5, 8, 9, 10, 12, 15, 20};
    std::vector<int> slow_periods = {20, 30, 40, 50, 60, 100};
    std::vector<double> adx_thresholds = {0.0, 20.0, 25.0, 30.0};
    std::vector<int> adx_windows = {14};
    size_t totalCombinations() const {
        return fast_periods.size() * slow_periods.size() * adx_thresholds.size() * adx_windows.size();
    }
    EmaCrossoverStrategy::Params operator[](size_t i) const {
        size_t aw = i % adx_windows.size(); i /= adx_windows.size();
        size_t at = i % adx_thresholds.size(); i /= adx_thresholds.size();
        size_t s = i % slow_periods.size(); i /= slow_periods.size();
        size_t f = i % fast_periods.size();
        return {fast_periods[f], slow_periods[s], adx_thresholds[at], adx_windows[aw]};
    }
};

} // namespace flox_my
