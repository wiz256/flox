#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class Rsi2Strategy : public flox::Strategy {
public:
    struct Params {
        int rsi_window;
        double oversold;
        double overbought;
        int sma_window;
        std::string toString() const {
            return "rsi_w=" + std::to_string(rsi_window) + ",os=" + std::to_string(oversold)
                 + ",ob=" + std::to_string(overbought) + ",sma=" + std::to_string(sma_window);
        }
    };

    Rsi2Strategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        size_t need = static_cast<size_t>(std::max(params_.rsi_window, params_.sma_window) + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        double rsi_val = rsi(closes_, params_.rsi_window);
        double sma_val = sma(closes_, params_.sma_window);
        if (std::isnan(rsi_val) || std::isnan(sma_val)) return;

        bool trend_up = closes_.back() > sma_val;

        if (!long_ && rsi_val < params_.oversold && trend_up) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (!short_ && rsi_val > params_.overbought && !trend_up) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && (rsi_val > params_.overbought || closes_.back() < sma_val)) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && (rsi_val < params_.oversold || closes_.back() > sma_val)) {
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

struct Rsi2Grid {
    std::vector<int> rsi_windows = {2, 3, 4, 5};
    std::vector<double> oversold_levels = {5.0, 10.0, 15.0, 20.0};
    std::vector<double> overbought_levels = {80.0, 85.0, 90.0, 95.0};
    std::vector<int> sma_windows = {5, 10, 20, 50};
    size_t totalCombinations() const {
        return rsi_windows.size() * oversold_levels.size() * overbought_levels.size() * sma_windows.size();
    }
    Rsi2Strategy::Params operator[](size_t i) const {
        size_t sw = i % sma_windows.size(); i /= sma_windows.size();
        size_t ob = i % overbought_levels.size(); i /= overbought_levels.size();
        size_t os = i % oversold_levels.size(); i /= oversold_levels.size();
        size_t rw = i % rsi_windows.size();
        return {rsi_windows[rw], oversold_levels[os], overbought_levels[ob], sma_windows[sw]};
    }
};

} // namespace flox_my
