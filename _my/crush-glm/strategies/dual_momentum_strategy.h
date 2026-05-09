#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class DualMomentumStrategy : public flox::Strategy {
public:
    struct Params {
        int lookback;
        double threshold;
        std::string toString() const {
            return "lb=" + std::to_string(lookback) + ",th=" + std::to_string(threshold);
        }
    };

    DualMomentumStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        if (closes_.size() > 500) closes_.pop_front();
        if (closes_.size() < static_cast<size_t>(params_.lookback + 2)) return;

        double mom = roc(closes_, params_.lookback);
        if (std::isnan(mom)) return;

        double mom_prev = 0;
        if (closes_.size() >= static_cast<size_t>(params_.lookback + 2)) {
            std::deque<double> tmp(closes_.begin(), closes_.end() - 1);
            mom_prev = roc(tmp, params_.lookback);
        }

        if (!long_ && mom > params_.threshold && mom_prev <= params_.threshold) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (!short_ && mom < -params_.threshold && mom_prev >= -params_.threshold) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && mom <= params_.threshold) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && mom >= -params_.threshold) {
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

struct DualMomentumGrid {
    std::vector<int> lookbacks = {10, 15, 20, 25, 30, 40, 50, 60};
    std::vector<double> thresholds = {0.0, 0.01, 0.02, 0.03, 0.05};
    size_t totalCombinations() const { return lookbacks.size() * thresholds.size(); }
    DualMomentumStrategy::Params operator[](size_t i) const {
        size_t t = i % thresholds.size(); i /= thresholds.size();
        size_t l = i % lookbacks.size();
        return {lookbacks[l], thresholds[t]};
    }
};

} // namespace flox_my
