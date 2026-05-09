#pragma once
#include "common.h"
#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"
#include "flox/strategy/strategy.h"
#include <deque>

namespace flox_my {

class KeltnerSqueezeStrategy : public flox::Strategy {
public:
    struct Params {
        int ema_window;
        int atr_window;
        double kc_mult;
        int bb_window;
        double bb_std;
        std::string toString() const {
            return "ema=" + std::to_string(ema_window) + ",atr=" + std::to_string(atr_window)
                 + ",kcm=" + std::to_string(kc_mult) + ",bbw=" + std::to_string(bb_window)
                 + ",bbs=" + std::to_string(bb_std);
        }
    };

    KeltnerSqueezeStrategy(flox::SymbolId symbol, const flox::SymbolRegistry& registry,
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
        size_t need = static_cast<size_t>(std::max({params_.ema_window, params_.atr_window, params_.bb_window}) + 2);
        if (closes_.size() > need * 2) closes_.pop_front();
        if (closes_.size() < need) return;

        auto kc = keltner(closes_, params_.ema_window, params_.atr_window, params_.kc_mult);
        auto bb = bollinger(closes_, params_.bb_window, params_.bb_std);
        if (std::isnan(kc.upper) || std::isnan(bb.upper)) return;

        bool squeeze_on = (bb.lower > kc.lower) && (bb.upper < kc.upper);
        bool squeeze_off = prev_squeeze_ && !squeeze_on;

        double cur = closes_.back();

        auto bb_prev = bollinger(std::deque<double>(closes_.begin(), std::prev(closes_.end())),
                                 params_.bb_window, params_.bb_std);

        if (squeeze_off && cur > bb_prev.upper && !long_) {
            emitMarketBuy(symbol(), size_);
            long_ = true; short_ = false;
        } else if (squeeze_off && cur < bb_prev.lower && !short_) {
            emitMarketSell(symbol(), size_);
            short_ = true; long_ = false;
        } else if (long_ && cur < kc.mid) {
            emitMarketSell(symbol(), size_);
            long_ = false;
        } else if (short_ && cur > kc.mid) {
            emitMarketBuy(symbol(), size_);
            short_ = false;
        }
        prev_squeeze_ = squeeze_on;
    }

private:
    Params params_;
    flox::Quantity size_;
    std::deque<double> closes_;
    bool running_{false}, prev_squeeze_{false}, long_{false}, short_{false};
};

struct KeltnerSqueezeGrid {
    std::vector<int> ema_windows = {15, 20, 25};
    std::vector<int> atr_windows = {10, 14, 21};
    std::vector<double> kc_mults = {1.5, 2.0, 2.5};
    std::vector<int> bb_windows = {15, 20, 25};
    std::vector<double> bb_stds = {1.5, 2.0, 2.5};
    size_t totalCombinations() const {
        return ema_windows.size() * atr_windows.size() * kc_mults.size()
             * bb_windows.size() * bb_stds.size();
    }
    KeltnerSqueezeStrategy::Params operator[](size_t i) const {
        size_t bs = i % bb_stds.size(); i /= bb_stds.size();
        size_t bw = i % bb_windows.size(); i /= bb_windows.size();
        size_t km = i % kc_mults.size(); i /= kc_mults.size();
        size_t aw = i % atr_windows.size(); i /= atr_windows.size();
        size_t ew = i % ema_windows.size();
        return {ema_windows[ew], atr_windows[aw], kc_mults[km], bb_windows[bw], bb_stds[bs]};
    }
};

} // namespace flox_my
