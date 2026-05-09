/*
 * Grid Search Main — 9 strategies with parallel execution
 *
 * Build:  cmake -B build && cmake --build build
 * Usage:  ./build/crush_grid data/BTC_4H.csv [num_threads]
 */

#include "strategies/strategies.h"
#include "csv_bar_reader.h"

#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/optimization_stats.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_tracker.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace flox;
using namespace flox::strategy;
using namespace flox::util;

// =============================================================================
// Combined parameter grid: all 9 strategies flattened into one index space
// =============================================================================

enum StrategyKind : int
{
    DONCHIAN_BREAKOUT = 0,
    DUAL_MOMENTUM,
    EMA_CROSSOVER,
    KELTNER_BREAKOUT,
    KELTNER_SQUEEZE,
    SUPERTREND,
    TSMOM,
    RSI2,
    RSI_BB_MR,
    NUM_STRATEGIES
};

inline const char* strategyName(StrategyKind k)
{
    static const char* names[] = {
        "donchian_breakout", "dual_momentum", "ema_crossover",
        "keltner_breakout", "keltner_squeeze", "supertrend",
        "tsmom", "rsi2", "rsi_bb_mr"};
    return names[k];
}

// We flatten all strategy params into a tagged union
struct CombinedParams
{
    StrategyKind kind;
    // Store up to 4 int params + 4 double params (sufficient for all strategies)
    int ip[4] = {};
    double dp[4] = {};

    std::string toString() const
    {
        std::string s = std::string(strategyName(kind)) + "(";
        for (int i = 0; i < 4; ++i)
        {
            if (i > 0) s += ",";
            s += std::to_string(ip[i]);
        }
        for (int i = 0; i < 4; ++i)
        {
            s += "," + std::to_string(dp[i]);
        }
        s += ")";
        return s;
    }
};

struct CombinedGrid
{
    // Per-strategy parameter ranges
    // Donchian: period=[10,20,40], atrSl=[1.5,2.0,2.5]
    // EMA: fast=[8,12,20], slow=[21,26,50]
    // Keltner: atrMult=[1.5,2.0,2.5], atrPeriod=[10,14,20]
    // Supertrend: period=[10,14], mult=[2.0,3.0,4.0]
    // RSI2: rsi=[2], sma=[100,200]
    // ... etc

    std::vector<CombinedParams> entries;

    CombinedGrid()
    {
        // Donchian Breakout: period x atrSl
        for (int period : {10, 20, 40})
            for (double sl : {1.5, 2.0, 2.5})
                entries.push_back({DONCHIAN_BREAKOUT, {period}, {sl}});

        // Dual Momentum: lookback x smaPeriod
        for (int lb : {6, 12, 24})
            for (int sma : {20, 50, 100})
                entries.push_back({DUAL_MOMENTUM, {lb, sma}});

        // EMA Crossover: fast x slow x atrSl
        for (int fast : {8, 12, 20})
            for (int slow : {21, 26, 50})
                if (fast < slow)
                    entries.push_back({EMA_CROSSOVER, {fast, slow}, {2.0}});

        // Keltner Breakout: ema x atr x mult x sl
        for (int ema : {20, 50})
            for (int atr : {10, 14})
                for (double mult : {1.5, 2.0, 2.5})
                    entries.push_back({KELTNER_BREAKOUT, {ema, atr}, {mult, 2.5}});

        // Keltner Squeeze: bb x kema x katr
        for (int bb : {20, 40})
            for (int kema : {20, 30})
                for (int katr : {10, 14})
                    entries.push_back({KELTNER_SQUEEZE, {bb, kema, katr}, {2.0, 1.5}});

        // Supertrend: period x mult
        for (int period : {10, 14, 20})
            for (double mult : {2.0, 3.0, 4.0})
                entries.push_back({SUPERTREND, {period}, {mult}});

        // TSMOM: lookback x volLookback
        for (int lb : {6, 12, 24})
            for (int vol : {20, 40})
                entries.push_back({TSMOM, {lb, vol}, {0.15}});

        // RSI2: rsi x sma x oversold x overbought
        for (int rsi : {2, 3})
            for (int sma : {100, 200})
                entries.push_back({RSI2, {rsi, sma}, {10.0, 90.0}});

        // RSI+BB MR: bb x rsi x bbStd x rsiOB/OS
        for (int bb : {20, 40})
            for (int rsi : {14, 21})
                entries.push_back({RSI_BB_MR, {bb, rsi}, {2.0, 30.0}});
    }

    size_t totalCombinations() const { return entries.size(); }
    CombinedParams operator[](size_t i) const { return entries[i]; }
};

// =============================================================================
// Backtest factory: creates strategy, runs backtest on cached bar events
// =============================================================================

static std::vector<BarEvent> g_bars;  // filled once in main()
static SymbolRegistry g_registry;
static SymbolId g_symbolId = 1;

BacktestResult runBacktest(const CombinedParams& p)
{
    BacktestConfig config;
    config.initialCapital = 10000.0;
    config.feeRate = 0.0004;  // 0.04% taker

    BacktestRunner runner(config);

    // Create the right strategy for these params
    Strategy* strat = nullptr;
    switch (p.kind)
    {
    case DONCHIAN_BREAKOUT:
        strat = new DonchianBreakoutStrategy(1, g_symbolId, g_registry,
            DonchianBreakoutParams{p.ip[0], p.dp[0]});
        break;
    case DUAL_MOMENTUM:
        strat = new DualMomentumStrategy(1, g_symbolId, g_registry,
            DualMomentumParams{p.ip[0], static_cast<double>(p.ip[1])});
        break;
    case EMA_CROSSOVER:
        strat = new EmaCrossoverStrategy(1, g_symbolId, g_registry,
            EmaCrossoverParams{p.ip[0], p.ip[1], p.dp[0]});
        break;
    case KELTNER_BREAKOUT:
        strat = new KeltnerBreakoutStrategy(1, g_symbolId, g_registry,
            KeltnerBreakoutParams{p.ip[0], p.ip[1], p.dp[0], p.dp[1]});
        break;
    case KELTNER_SQUEEZE:
        strat = new KeltnerSqueezeStrategy(1, g_symbolId, g_registry,
            KeltnerSqueezeParams{p.ip[0], p.dp[0], p.ip[1], p.ip[2], p.dp[1]});
        break;
    case SUPERTREND:
        strat = new SupertrendStrategy(1, g_symbolId, g_registry,
            SupertrendParams{p.ip[0], p.dp[0]});
        break;
    case TSMOM:
        strat = new TsmomStrategy(1, g_symbolId, g_registry,
            TsmomParams{p.ip[0], static_cast<double>(p.ip[1]), p.dp[0]});
        break;
    case RSI2:
        strat = new Rsi2Strategy(1, g_symbolId, g_registry,
            Rsi2Params{p.ip[0], p.ip[1], p.dp[0], p.dp[1]});
        break;
    case RSI_BB_MR:
        strat = new RsiBbMrStrategy(1, g_symbolId, g_registry,
            RsiBbMrParams{p.ip[0], p.dp[0], p.ip[1], p.dp[1], 100.0 - p.dp[1]});
        break;
    default:
        strat = new EmaCrossoverStrategy(1, g_symbolId, g_registry);
        break;
    }

    runner.setStrategy(strat);
    BacktestResult result = runner.runBars(g_bars);
    delete strat;
    return result;
}

// =============================================================================
// Multi-threaded grid search (fixes the sequential-only runLocal)
// =============================================================================

std::vector<OptimizationResult<CombinedParams>> runParallel(
    const CombinedGrid& grid, size_t numThreads)
{
    size_t total = grid.totalCombinations();
    std::vector<OptimizationResult<CombinedParams>> results(total);
    std::atomic<size_t> nextIdx{0};
    std::atomic<size_t> completed{0};

    auto worker = [&]()
    {
        while (true)
        {
            size_t i = nextIdx.fetch_add(1);
            if (i >= total) break;

            BacktestResult bt = runBacktest(grid[i]);
            auto stats = bt.computeStats();
            results[i].parameters = grid[i];
            results[i].setFromStats(stats);

            size_t done = ++completed;
            if (done % 10 == 0 || done == total)
            {
                std::cerr << "\rProgress: " << done << "/" << total
                          << " (" << (done * 100 / total) << "%)" << std::flush;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (size_t t = 0; t < numThreads; ++t)
        threads.emplace_back(worker);
    for (auto& th : threads)
        th.join();

    std::cerr << "\n";
    return results;
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <data.csv> [num_threads]\n";
        std::cerr << "  data.csv: OHLCV CSV file (timestamp,open,high,low,close,volume)\n";
        return 1;
    }

    std::string csvPath = argv[1];
    size_t numThreads = (argc > 2) ? std::stoul(argv[2]) : std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 1;

    // Load CSV data
    std::cerr << "Loading " << csvPath << "...\n";
    auto csvBars = readCsvBars(csvPath);
    if (csvBars.empty())
    {
        std::cerr << "Error: no bars loaded from " << csvPath << "\n";
        return 1;
    }
    std::cerr << "Loaded " << csvBars.size() << " bars\n";

    // Register symbol
    SymbolInfo info;
    info.id = g_symbolId;
    info.exchange = "BACKTEST";
    info.symbol = "ASSET";
    info.tickSize = Price::fromDouble(0.01);
    g_registry.registerSymbol(info);

    // Convert to BarEvents
    g_bars = csvBarsToBarEvents(csvBars, g_symbolId);

    // Build grid
    CombinedGrid grid;
    std::cerr << "Grid: " << grid.totalCombinations() << " parameter combinations\n";
    std::cerr << "Using " << numThreads << " threads\n";

    // Run parallel grid search
    auto results = runParallel(grid, numThreads);

    // Rank and print results
    auto ranked = BacktestOptimizer<CombinedParams, CombinedGrid>::rankResults(
        results, RankMetric::SharpeRatio);

    std::cout << "\n=== Top 20 Results (by Sharpe) ===\n";
    for (size_t i = 0; i < std::min(size_t(20), ranked.size()); ++i)
    {
        const auto& r = ranked[i];
        if (r.totalTrades() < 5) continue;
        std::cout << i + 1 << ". " << r.parameters.toString() << "\n"
                  << "   Sharpe: " << r.sharpeRatio()
                  << " | Return: " << r.totalReturn()
                  << " | MaxDD: " << r.maxDrawdownPct() << "%"
                  << " | Trades: " << r.totalTrades()
                  << " | Win%: " << (r.winRate() * 100) << "\n";
    }

    // Statistical summary
    using Stats = OptimizationStatistics<CombinedParams, CombinedGrid>;
    Stats::printSummary(results);

    // Export
    BacktestOptimizer<CombinedParams, CombinedGrid>::exportToCSV(
        ranked, "grid_search_results.csv");
    std::cerr << "Results exported to grid_search_results.csv\n";

    return 0;
}
