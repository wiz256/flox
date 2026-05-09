#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class SupertrendStrategy : public flox::Strategy {
public:
    struct Params {
        int atr_window;
        double atr_mult;
        std::string toString() const {
            return "atr_w=" + std::to_string(atr_window) + ",atr_m=" + std::to_string(atr_mult);
        }
    };

    SupertrendStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        size_t need = static_cast<size_t>(params_.atr_window + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        auto st = supertrend(closes_, params_.atr_window, params_.atr_mult);
        if (std::isnan(st.value)) return;

        int dir = st.direction;
        if (dir == 1 && prev_dir_ == -1 && !long_) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (dir == -1 && prev_dir_ == 1 && !short_) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && dir == -1) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && dir == 1) {
            emitMarketBuy(symbol(), size_);
            short_ = false;
        }
        prev_dir_ = dir;
    }

private:
    Params params_;
    flox::Quantity size_;
    std::deque<double> closes_;
    bool running_{false}, long_{false}, short_{false};
    int prev_dir_{0};
};

struct SupertrendGrid {
    std::vector<int> atr_windows = {7, 10, 14, 21};
    std::vector<double> atr_mults = {2.0, 2.5, 3.0, 3.5, 4.0};
    size_t totalCombinations() const { return atr_windows.size() * atr_mults.size(); }
    SupertrendStrategy::Params operator[](size_t i) const {
        size_t m = i % atr_mults.size(); i /= atr_mults.size();
        size_t w = i % atr_windows.size();
        return {atr_windows[w], atr_mults[m]};
    }
};

} // namespace flox_my
