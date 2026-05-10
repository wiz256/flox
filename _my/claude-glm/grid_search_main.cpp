/*
 * Grid Search Main — 13 strategies with exact Python grid params, multi-threaded
 *
 * Build:  ./build.sh
 * Usage:  ./build/crush_grid data/BTCUSDT_4h.csv [num_threads]
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

using namespace flox;
using namespace flox::strategy;
using namespace flox::util;

// =============================================================================
// Strategy registry
// =============================================================================

enum StrategyKind : int
{
    BOLLINGER_BREAKOUT = 0,
    DONCHIAN_BREAKOUT,
    DUAL_MOMENTUM,
    EMA_CROSSOVER,
    KELTNER_BREAKOUT,
    KELTNER_SQUEEZE,
    MACD,
    RSI_BB_MR,
    RSI2,
    SUPERTREND,
    TREND_PULLBACK,
    TSMOM,
    VOL_COMPRESSION_BREAKOUT,
    NUM_STRATEGIES
};

inline const char* strategyName(StrategyKind k)
{
    static const char* names[] = {
        "bollinger_breakout", "donchian", "dual_momentum", "ema_crossover",
        "keltner_breakout", "keltner_squeeze", "macd", "rsi_bb_mr", "rsi2",
        "supertrend", "trend_pullback", "tsmom", "vol_compression_breakout"};
    return names[k];
}

// Flattened params: kind + up to 6 ints + 6 doubles
struct CombinedParams
{
    StrategyKind kind;
    int ip[6] = {};
    double dp[6] = {};
    std::string toString() const
    {
        return std::string(strategyName(kind));
    }
};

// =============================================================================
// Grid builder — matches Python PARAM_GRID exactly
// =============================================================================

struct CombinedGrid
{
    std::vector<CombinedParams> entries;

    CombinedGrid()
    {
        // 1. Bollinger Breakout: bb_period=[10,20,30,50], bb_std=[1.5,2.0,2.5,3.0], vol_mult=[0.0,1.3,1.5]
        for (int bbp : {10, 20, 30, 50})
            for (double bbs : {1.5, 2.0, 2.5, 3.0})
                for (double vm : {0.0, 1.3, 1.5})
                    entries.push_back({BOLLINGER_BREAKOUT, {bbp}, {bbs, vm}});

        // 2. Donchian: channel_period=[10,15,20,30,40,55], exit_period=[5,8,10,15,20], vol_mult=[0.0,1.3,1.5,2.0]
        for (int cp : {10, 15, 20, 30, 40, 55})
            for (int ep : {5, 8, 10, 15, 20})
                for (double vm : {0.0, 1.3, 1.5, 2.0})
                    entries.push_back({DONCHIAN_BREAKOUT, {cp, ep}, {vm}});

        // 3. Dual Momentum: lookback=[20,40,60,120], mom_threshold=[0.0,0.02,0.05], smooth=[1,3,5]
        for (int lb : {20, 40, 60, 120})
            for (double mt : {0.0, 0.02, 0.05})
                for (int sm : {1, 3, 5})
                    entries.push_back({DUAL_MOMENTUM, {lb, sm}, {mt}});

        // 4. EMA Crossover: fast=[5,8,13,21,34], slow=[21,34,55,89,144], adx_min=[0,15,20,25]
        for (int f : {5, 8, 13, 21, 34})
            for (int s : {21, 34, 55, 89, 144})
                if (f < s)
                    for (int adx : {0, 15, 20, 25})
                        entries.push_back({EMA_CROSSOVER, {f, s, adx}});

        // 5. Keltner Breakout: ema=[10,20,30,40,50,60,80], atr=[10,14,20,30],
        //    atr_mult=[2.0,2.5,2.8,3.0,3.2,3.5], atr_pct_min=[0.0,0.005,0.008]
        for (int ema : {10, 20, 30, 40, 50, 60, 80})
            for (int atr : {10, 14, 20, 30})
                for (double am : {2.0, 2.5, 2.8, 3.0, 3.2, 3.5})
                    for (double apm : {0.0, 0.005, 0.008})
                        entries.push_back({KELTNER_BREAKOUT, {ema, atr}, {am, apm}});

        // 6. Keltner Squeeze: ema=[10,20,30], kelt_mult=[1.5,2.0,2.5], bb=[10,20,30], bb_std=[1.5,2.0,2.5]
        for (int ema : {10, 20, 30})
            for (double km : {1.5, 2.0, 2.5})
                for (int bb : {10, 20, 30})
                    for (double bs : {1.5, 2.0, 2.5})
                        entries.push_back({KELTNER_SQUEEZE, {ema, bb}, {km, bs}});

        // 7. MACD: fast=[8,12,16], slow=[21,26,34], signal=[7,9,12], trend=[0,100,200]
        for (int f : {8, 12, 16})
            for (int s : {21, 26, 34})
                if (f < s)
                    for (int sig : {7, 9, 12})
                        for (int tp : {0, 100, 200})
                            entries.push_back({MACD, {f, s, sig, tp}});

        // 8. RSI+BB MR: rsi_period=[7,14,21], rsi_low=[20,25,30], rsi_high=[70,75,80],
        //    bb_period=[15,20,30], bb_std=[1.5,2.0,2.5]
        for (int rp : {7, 14, 21})
            for (double rl : {20.0, 25.0, 30.0})
                for (double rh : {70.0, 75.0, 80.0})
                    for (int bb : {15, 20, 30})
                        for (double bs : {1.5, 2.0, 2.5})
                            entries.push_back({RSI_BB_MR, {rp, bb}, {rl, rh, bs}});

        // 9. RSI-2: rsi_period=[2,3], entry_low=[5,10,15], entry_high=[85,90,95], trend=[50,100,200]
        for (int rp : {2, 3})
            for (double el : {5.0, 10.0, 15.0})
                for (double eh : {85.0, 90.0, 95.0})
                    for (int tp : {50, 100, 200})
                        entries.push_back({RSI2, {rp, tp}, {el, eh}});

        // 10. Supertrend: atr_period=[7,10,14,20], atr_mult=[2.0,2.5,3.0,3.5,4.0], adx_min=[0,20,25]
        for (int ap : {7, 10, 14, 20})
            for (double am : {2.0, 2.5, 3.0, 3.5, 4.0})
                for (int adx : {0, 20, 25})
                    entries.push_back({SUPERTREND, {ap, adx}, {am}});

        // 11. Trend Pullback: fast_ema=[10,20,30], slow_ema=[50,100,150,200], rsi_pb=[35,40,45], atr_pct_min=[0.0,0.004,0.008]
        for (int fe : {10, 20, 30})
            for (int se : {50, 100, 150, 200})
                if (fe < se)
                    for (int rp : {35, 40, 45})
                        for (double apm : {0.0, 0.004, 0.008})
                            entries.push_back({TREND_PULLBACK, {fe, se, rp}, {apm}});

        // 12. TSMOM: lookback=[20,40,60,80,120,160], smooth=[1,3,5,10], atr_pct_min=[0.0,0.005,0.008]
        for (int lb : {20, 40, 60, 80, 120, 160})
            for (int sm : {1, 3, 5, 10})
                for (double apm : {0.0, 0.005, 0.008})
                    entries.push_back({TSMOM, {lb, sm}, {apm}});

        // 13. Vol Compression Breakout: range_window=[20,30,40,60], compression_window=[60,100,150],
        //     compression_pct=[0.2,0.3,0.4], vol_mult=[0.0,1.3,1.5]
        for (int rw : {20, 30, 40, 60})
            for (int cw : {60, 100, 150})
                if (rw < cw)
                    for (double cp : {0.2, 0.3, 0.4})
                        for (double vm : {0.0, 1.3, 1.5})
                            entries.push_back({VOL_COMPRESSION_BREAKOUT, {rw, cw}, {cp, vm}});
    }

    size_t totalCombinations() const { return entries.size(); }
    CombinedParams operator[](size_t i) const { return entries[i]; }
};

// =============================================================================
// Shared state
// =============================================================================

static std::vector<BarEvent> g_bars;
static SymbolRegistry g_registry;
static SymbolId g_symbolId = 1;

// =============================================================================
// Strategy factory from CombinedParams
// =============================================================================

Strategy* createStrategy(const CombinedParams& p)
{
    switch (p.kind)
    {
    case BOLLINGER_BREAKOUT:
        return new BollingerBreakoutStrategy(1, g_symbolId, g_registry,
            BollingerBreakoutParams{p.ip[0], p.dp[0], p.dp[1]});
    case DONCHIAN_BREAKOUT:
        return new DonchianBreakoutStrategy(1, g_symbolId, g_registry,
            DonchianBreakoutParams{p.ip[0], p.ip[1], p.dp[0]});
    case DUAL_MOMENTUM:
        return new DualMomentumStrategy(1, g_symbolId, g_registry,
            DualMomentumParams{p.ip[0], p.dp[0], p.ip[1]});
    case EMA_CROSSOVER:
        return new EmaCrossoverStrategy(1, g_symbolId, g_registry,
            EmaCrossoverParams{p.ip[0], p.ip[1], p.ip[2]});
    case KELTNER_BREAKOUT:
        return new KeltnerBreakoutStrategy(1, g_symbolId, g_registry,
            KeltnerBreakoutParams{p.ip[0], p.ip[1], p.dp[0], p.dp[1]});
    case KELTNER_SQUEEZE:
        return new KeltnerSqueezeStrategy(1, g_symbolId, g_registry,
            KeltnerSqueezeParams{p.ip[0], p.dp[0], p.ip[1], p.dp[1]});
    case MACD:
        return new MacdStrategy(1, g_symbolId, g_registry,
            MacdParams{p.ip[0], p.ip[1], p.ip[2], p.ip[3]});
    case RSI_BB_MR:
        return new RsiBbMrStrategy(1, g_symbolId, g_registry,
            RsiBbMrParams{p.ip[0], p.dp[0], p.dp[1], p.ip[1], p.dp[2]});
    case RSI2:
        return new Rsi2Strategy(1, g_symbolId, g_registry,
            Rsi2Params{p.ip[0], p.dp[0], p.dp[1], p.ip[1]});
    case SUPERTREND:
        return new SupertrendStrategy(1, g_symbolId, g_registry,
            SupertrendParams{p.ip[0], p.dp[0], p.ip[1]});
    case TREND_PULLBACK:
        return new TrendPullbackStrategy(1, g_symbolId, g_registry,
            TrendPullbackParams{p.ip[0], p.ip[1], p.ip[2], p.dp[0]});
    case TSMOM:
        return new TsmomStrategy(1, g_symbolId, g_registry,
            TsmomParams{p.ip[0], p.ip[1], p.dp[0]});
    case VOL_COMPRESSION_BREAKOUT:
        return new VolCompressionBreakoutStrategy(1, g_symbolId, g_registry,
            VolCompressionBreakoutParams{p.ip[0], p.ip[1], p.dp[0], p.dp[1]});
    default:
        return new EmaCrossoverStrategy(1, g_symbolId, g_registry);
    }
}

BacktestResult runBacktest(const CombinedParams& p)
{
    BacktestConfig config;
    config.initialCapital = 10000.0;
    config.feeRate = 0.0004;

    BacktestRunner runner(config);
    Strategy* strat = createStrategy(p);
    runner.setStrategy(strat);
    BacktestResult result = runner.runBars(g_bars);
    delete strat;
    return result;
}

// =============================================================================
// Multi-threaded grid search
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
            if (done % 50 == 0 || done == total)
            {
                std::cerr << "\rProgress: " << done << "/" << total
                          << " (" << (done * 100 / total) << "%)" << std::flush;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (size_t t = 0; t < numThreads; ++t) threads.emplace_back(worker);
    for (auto& th : threads) th.join();
    std::cerr << "\n";
    return results;
}

// =============================================================================
// Per-strategy summary
// =============================================================================

void printPerStrategySummary(const std::vector<OptimizationResult<CombinedParams>>& results)
{
    struct StratStats { size_t count = 0; double bestSharpe = -999; double avgSharpe = 0; double bestReturn = -999; std::string bestParams; };
    StratStats stats[NUM_STRATEGIES];

    for (const auto& r : results)
    {
        int k = static_cast<int>(r.parameters.kind);
        if (k < 0 || k >= NUM_STRATEGIES) continue;
        auto& s = stats[k];
        s.count++;
        s.avgSharpe += r.sharpeRatio();
        if (r.sharpeRatio() > s.bestSharpe) {
            s.bestSharpe = r.sharpeRatio();
            s.bestReturn = r.totalReturn();
            s.bestParams = r.parameters.toString();
        }
    }

    std::cout << "\n=== Per-Strategy Summary ===\n";
    std::cout << "Strategy                   | Combos | Best Sharpe | Best Return | Avg Sharpe\n";
    std::cout << "---------------------------|--------|-------------|-------------|-----------\n";
    for (int k = 0; k < NUM_STRATEGIES; ++k)
    {
        auto& s = stats[k];
        double avg = s.count > 0 ? s.avgSharpe / static_cast<double>(s.count) : 0.0;
        printf("%-26s | %6zu | %11.2f | %11.1f%% | %9.2f\n",
               strategyName(static_cast<StrategyKind>(k)), s.count, s.bestSharpe, s.bestReturn, avg);
    }
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <data.csv> [num_threads]\n";
        return 1;
    }

    std::string csvPath = argv[1];
    size_t numThreads = (argc > 2) ? std::stoul(argv[2]) : std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 1;

    std::cerr << "Loading " << csvPath << "...\n";
    auto csvBars = readCsvBars(csvPath);
    if (csvBars.empty()) { std::cerr << "Error: no bars loaded\n"; return 1; }
    std::cerr << "Loaded " << csvBars.size() << " bars\n";

    SymbolInfo info;
    info.id = g_symbolId;
    info.exchange = "BACKTEST";
    info.symbol = "ASSET";
    info.tickSize = Price::fromDouble(0.01);
    g_registry.registerSymbol(info);

    g_bars = csvBarsToBarEvents(csvBars, g_symbolId);

    CombinedGrid grid;
    std::cerr << "Grid: " << grid.totalCombinations() << " combinations across " << NUM_STRATEGIES << " strategies\n";
    std::cerr << "Using " << numThreads << " threads\n\n";

    auto results = runParallel(grid, numThreads);

    // Global ranking
    auto ranked = BacktestOptimizer<CombinedParams, CombinedGrid>::rankResults(results, RankMetric::SharpeRatio);

    std::cout << "\n=== Top 30 Results (by Sharpe, min 5 trades) ===\n";
    int shown = 0;
    for (size_t i = 0; i < ranked.size() && shown < 30; ++i)
    {
        if (ranked[i].totalTrades() < 5) continue;
        std::cout << shown + 1 << ". " << ranked[i].parameters.toString() << "\n"
                  << "   Sharpe: " << ranked[i].sharpeRatio()
                  << " | Return: " << ranked[i].totalReturn()
                  << "% | MaxDD: " << ranked[i].maxDrawdownPct() << "%"
                  << " | Trades: " << ranked[i].totalTrades()
                  << " | Win%: " << (ranked[i].winRate() * 100) << "\n";
        ++shown;
    }

    // Per-strategy breakdown
    printPerStrategySummary(results);

    // Statistical summary
    using Stats = OptimizationStatistics<CombinedParams, CombinedGrid>;
    Stats::printSummary(results);

    // Export
    BacktestOptimizer<CombinedParams, CombinedGrid>::exportToCSV(ranked, "grid_search_results.csv");
    std::cerr << "\nResults exported to grid_search_results.csv\n";

    return 0;
}
