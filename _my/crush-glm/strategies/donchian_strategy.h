#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>
#include <iostream>

namespace flox_my {

class DonchianStrategy : public flox::Strategy {
public:
    struct Params {
        int window;
        int atr_window;
        double sl_atr_mult;
        std::string toString() const {
            return "win=" + std::to_string(window) + ",atr=" + std::to_string(atr_window)
                 + ",sl=" + std::to_string(sl_atr_mult);
        }
    };

    DonchianStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        if (closes_.size() > static_cast<size_t>(params_.window * 2 + params_.atr_window + 10))
            closes_.pop_front();

        if (closes_.size() < static_cast<size_t>(params_.window + 1)) return;

        double cur = closes_.back();
        double upper = *std::max_element(
            closes_.end() - static_cast<ptrdiff_t>(params_.window) - 1,
            closes_.end() - 1);
        double lower = *std::min_element(
            closes_.end() - static_cast<ptrdiff_t>(params_.window) - 1,
            closes_.end() - 1);

        if (!long_ && cur > upper) {
            emitMarketBuy(symbol(), size_);
            long_ = true;
            short_ = false;
            entry_price_ = cur;
        } else if (!short_ && cur < lower) {
            emitMarketSell(symbol(), size_);
            short_ = true;
            long_ = false;
            entry_price_ = cur;
        } else if (long_ && cur < lower) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && cur > upper) {
            emitMarketBuy(symbol(), size_);
            short_ = false;
        }
    }

private:
    Params params_;
    flox::Quantity size_;
    std::deque<double> closes_;
    bool running_{false}, long_{false}, short_{false};
    double entry_price_{0};
};

struct DonchianGrid {
    std::vector<int> windows = {10, 15, 20, 25, 30, 40, 50, 55};
    std::vector<int> atr_windows = {10, 14, 21};
    std::vector<double> sl_mults = {1.0, 1.5, 2.0, 2.5, 3.0};
    size_t totalCombinations() const { return windows.size() * atr_windows.size() * sl_mults.size(); }
    DonchianStrategy::Params operator[](size_t i) const {
        size_t s = i % sl_mults.size(); i /= sl_mults.size();
        size_t a = i % atr_windows.size(); i /= atr_windows.size();
        size_t w = i % windows.size();
        return {windows[w], atr_windows[a], sl_mults[s]};
    }
};

} // namespace flox_my
