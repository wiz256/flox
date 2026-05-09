#include "../strategies/donchian_strategy.h"
#include "../strategies/dual_momentum_strategy.h"
#include "../strategies/ema_crossover_strategy.h"
#include "../strategies/keltner_breakout_strategy.h"
#include "../strategies/keltner_squeeze_strategy.h"
#include "../strategies/supertrend_strategy.h"
#include "../strategies/tsmom_strategy.h"
#include "../strategies/rsi2_strategy.h"
#include "../strategies/rsi_bb_mr_strategy.h"

#include "flox/backtest/backtest_config.h"
#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/engine/symbol_registry.h"
#include "flox/replay/readers/csv_ohlcv_reader.h"

#include <filesystem>
#include <fmt/format.h>
#include <iostream>
#include <string>
#include <vector>

using namespace flox;
using namespace flox_my;

static const std::string DATA_DIR = "vendor/flox/_my/data";
static const double TRADE_SIZE = 0.01;
static const double FEE_RATE = 0.0004;
static const double INITIAL_CAPITAL = 10000.0;

template<typename StratT, typename GridT>
void run_grid(const std::string& name, const std::string& csv_path, const GridT& grid) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Strategy: " << name << "\n";
    std::cout << "Data: " << csv_path << "\n";
    std::cout << "Grid combinations: " << grid.totalCombinations() << "\n";
    std::cout << std::string(60, '-') << "\n";

    if (!std::filesystem::exists(csv_path)) {
        std::cout << "  SKIP: data file not found\n";
        return;
    }

    SymbolRegistry registry;
    SymbolInfo info{.exchange = "binance", .symbol = "BTCUSDT", .tickSize = Price::fromDouble(0.01)};
    auto symId = registry.registerSymbol(info);

    using Params = typename StratT::Params;

    BacktestOptimizer<Params, GridT> optimizer;
    optimizer.setParameterGrid(grid);
    optimizer.setBacktestFactory([&](const Params& p) -> BacktestResult {
        auto reader = replay::createCsvOhlcvReader(csv_path, symId);
        BacktestConfig config{
            .initialCapital = INITIAL_CAPITAL,
            .feeRate = FEE_RATE,
        };
        BacktestRunner runner(config);
        StratT strat(symId, registry, p, TRADE_SIZE);
        runner.setStrategy(&strat);
        return runner.run(*reader);
    });

    optimizer.setProgressCallback(
        [](size_t done, size_t total, const OptimizationResult<Params>& latest) {
            std::cout << "\r  [" << done << "/" << total << "]"
                      << " Sharpe: " << latest.sharpeRatio() << std::flush;
        });

    auto results = optimizer.runLocal();
    auto ranked = BacktestOptimizer<Params, GridT>::rankResults(results, RankMetric::SharpeRatio);

    std::cout << "\n\n  Top 5 results:\n";
    for (size_t i = 0; i < std::min(size_t(5), ranked.size()); ++i) {
        const auto& r = ranked[i];
        std::cout << "  " << (i+1) << ". " << r.parameters.toString()
                  << " | Sharpe: " << r.sharpeRatio()
                  << " | Return: " << r.totalReturn() << "%"
                  << " | Trades: " << r.totalTrades()
                  << " | Win%: " << (r.winRate() * 100) << "\n";
    }

    std::string csv_out = "vendor/flox/_my/data/results_" + name + "_grid.csv";
    BacktestOptimizer<Params, GridT>::exportToCSV(ranked, csv_out);
    std::cout << "  Results saved to: " << csv_out << "\n";
}

int main(int argc, char** argv) {
    std::vector<std::string> symbols = {"BTCUSDTUSDT"};
    std::vector<std::string> timeframes = {"4h"};

    if (argc > 1) symbols = {argv[1]};
    if (argc > 2) timeframes = {argv[2]};

    for (const auto& sym : symbols) {
        for (const auto& tf : timeframes) {
            std::string csv = fmt::format("{}/{}_{}.csv", DATA_DIR, sym, tf);
            std::cout << "\n>>> Running all grid searches on " << csv << "\n";

            run_grid<DonchianStrategy>("donchian", csv, DonchianGrid{});
            run_grid<DualMomentumStrategy>("dual_momentum", csv, DualMomentumGrid{});
            run_grid<EmaCrossoverStrategy>("ema_crossover", csv, EmaCrossoverGrid{});
            run_grid<KeltnerBreakoutStrategy>("keltner_breakout", csv, KeltnerBreakoutGrid{});
            run_grid<KeltnerSqueezeStrategy>("keltner_squeeze", csv, KeltnerSqueezeGrid{});
            run_grid<SupertrendStrategy>("supertrend", csv, SupertrendGrid{});
            run_grid<TsmomStrategy>("tsmom", csv, TsmomGrid{});
            run_grid<Rsi2Strategy>("rsi2", csv, Rsi2Grid{});
            run_grid<RsiBbMrStrategy>("rsi_bb_mr", csv, RsiBbMrGrid{});
        }
    }

    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "All grid searches complete.\n";
    return 0;
}
