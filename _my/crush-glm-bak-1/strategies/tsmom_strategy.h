#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class TsmomStrategy : public flox::Strategy {
public:
    struct Params {
        int lookback;
        int vol_window;
        double vol_target;
        std::string toString() const {
            return "lb=" + std::to_string(lookback) + ",vw=" + std::to_string(vol_window)
                 + ",vt=" + std::to_string(vol_target);
        }
    };

    TsmomStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        size_t need = static_cast<size_t>(std::max(params_.lookback, params_.vol_window) + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        double mom = roc(closes_, params_.lookback);
        if (std::isnan(mom)) return;

        std::deque<double> returns;
        for (size_t i = 1; i < closes_.size(); ++i)
            returns.push_back(closes_[i] / closes_[i-1] - 1.0);
        if (returns.size() < static_cast<size_t>(params_.vol_window)) return;

        double vol = rolling_std(returns, params_.vol_window) * std::sqrt(252.0);
        bool vol_ok = std::isnan(vol) || vol < params_.vol_target;

        double mom_prev = 0;
        if (closes_.size() >= static_cast<size_t>(params_.lookback + 2)) {
            std::deque<double> tmp(closes_.begin(), closes_.end() - 1);
            mom_prev = roc(tmp, params_.lookback);
        }

        if (vol_ok && mom > 0 && mom_prev <= 0 && !long_) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (vol_ok && mom < 0 && mom_prev >= 0 && !short_) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && mom <= 0) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && mom >= 0) {
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

struct TsmomGrid {
    std::vector<int> lookbacks = {10, 15, 20, 25, 30, 50};
    std::vector<int> vol_windows = {10, 15, 20, 30};
    std::vector<double> vol_targets = {0.10, 0.15, 0.20, 0.25, 0.30};
    size_t totalCombinations() const { return lookbacks.size() * vol_windows.size() * vol_targets.size(); }
    TsmomStrategy::Params operator[](size_t i) const {
        size_t vt = i % vol_targets.size(); i /= vol_targets.size();
        size_t vw = i % vol_windows.size(); i /= vol_windows.size();
        size_t lb = i % lookbacks.size();
        return {lookbacks[lb], vol_windows[vw], vol_targets[vt]};
    }
};

} // namespace flox_my
