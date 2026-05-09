#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class KeltnerBreakoutStrategy : public flox::Strategy {
public:
    struct Params {
        int ema_window;
        int atr_window;
        double atr_mult;
        std::string toString() const {
            return "ema=" + std::to_string(ema_window) + ",atr_w=" + std::to_string(atr_window)
                 + ",atr_m=" + std::to_string(atr_mult);
        }
    };

    KeltnerBreakoutStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        size_t need = static_cast<size_t>(std::max(params_.ema_window, params_.atr_window) + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        auto kc = keltner(closes_, params_.ema_window, params_.atr_window, params_.atr_mult);
        if (std::isnan(kc.upper)) return;

        auto kc_prev = keltner(std::deque<double>(closes_.begin(), std::prev(closes_.end())),
                               params_.ema_window, params_.atr_window, params_.atr_mult);

        double cur = closes_.back();

        if (!long_ && cur > kc_prev.upper) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (!short_ && cur < kc_prev.lower) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && cur < kc_prev.mid) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && cur > kc_prev.mid) {
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

struct KeltnerBreakoutGrid {
    std::vector<int> ema_windows = {10, 15, 20, 25, 30};
    std::vector<int> atr_windows = {10, 14, 21};
    std::vector<double> atr_mults = {1.5, 2.0, 2.5, 3.0};
    size_t totalCombinations() const { return ema_windows.size() * atr_windows.size() * atr_mults.size(); }
    KeltnerBreakoutStrategy::Params operator[](size_t i) const {
        size_t m = i % atr_mults.size(); i /= atr_mults.size();
        size_t a = i % atr_windows.size(); i /= atr_windows.size();
        size_t e = i % ema_windows.size();
        return {ema_windows[e], atr_windows[a], atr_mults[m]};
    }
};

} // namespace flox_my
