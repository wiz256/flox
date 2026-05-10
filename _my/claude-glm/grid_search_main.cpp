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
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
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
        std::ostringstream ss;
        ss << strategyName(kind) << "(";
        switch (kind) {
        case BOLLINGER_BREAKOUT:
            ss << "bb=" << ip[0] << ",std=" << dp[0] << ",vm=" << dp[1]; break;
        case DONCHIAN_BREAKOUT:
            ss << "cp=" << ip[0] << ",ep=" << ip[1] << ",vm=" << dp[0]; break;
        case DUAL_MOMENTUM:
            ss << "lb=" << ip[0] << ",mt=" << dp[0] << ",sm=" << ip[1]; break;
        case EMA_CROSSOVER:
            ss << "f=" << ip[0] << ",s=" << ip[1] << ",adx=" << ip[2]; break;
        case KELTNER_BREAKOUT:
            ss << "ema=" << ip[0] << ",atr=" << ip[1] << ",am=" << dp[0] << ",apm=" << dp[1]; break;
        case KELTNER_SQUEEZE:
            ss << "ke=" << ip[0] << ",km=" << dp[0] << ",bp=" << ip[1] << ",bs=" << dp[1]; break;
        case MACD:
            ss << "f=" << ip[0] << ",s=" << ip[1] << ",sig=" << ip[2] << ",tp=" << ip[3]; break;
        case RSI_BB_MR:
            ss << "rp=" << ip[0] << ",rl=" << dp[0] << ",rh=" << dp[1] << ",bp=" << ip[1] << ",bs=" << dp[2]; break;
        case RSI2:
            ss << "rp=" << ip[0] << ",el=" << dp[0] << ",eh=" << dp[1] << ",tp=" << ip[1]; break;
        case SUPERTREND:
            ss << "ap=" << ip[0] << ",am=" << dp[0] << ",adx=" << ip[1]; break;
        case TREND_PULLBACK:
            ss << "fe=" << ip[0] << ",se=" << ip[1] << ",rp=" << ip[2] << ",apm=" << dp[0]; break;
        case TSMOM:
            ss << "lb=" << ip[0] << ",sm=" << ip[1] << ",apm=" << dp[0]; break;
        case VOL_COMPRESSION_BREAKOUT:
            ss << "rw=" << ip[0] << ",cw=" << ip[1] << ",cp=" << dp[0] << ",vm=" << dp[1]; break;
        default: break;
        }
        ss << ")";
        return ss.str();
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
static double g_initialCapital = 10000.0;
static double g_feeRate = 0.0004;

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
    config.initialCapital = g_initialCapital;
    config.feeRate = g_feeRate;

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
        std::cerr << "Usage: " << argv[0] << " <data.csv> [options]\n"
                  << "  --sizing-mode <mode>    all_equity|fixed_notional|percent_equity (default: fixed_notional)\n"
                  << "  --notional <usd>        Notional per trade for fixed_notional mode (default: 1000)\n"
                  << "  --percent-equity <pct>  Equity fraction for percent_equity mode (default: 0.01 = 1%%)\n"
                  << "  --leverage <mult>       Leverage multiplier for all modes (default: 1.0)\n"
                  << "  --min-trades <n>        Minimum trades filter (default: 30)\n"
                  << "  --threads <n>           Number of threads (default: all cores)\n"
                  << "  --capital <usd>         Initial capital (default: 10000)\n"
                  << "  --fee <rate>            Fee rate (default: 0.0004 = 0.04%%)\n";
        return 1;
    }

    // Parse arguments
    std::string csvPath = argv[1];
    SizingMode sizingMode = SizingMode::FIXED_NOTIONAL;
    double notional = 1000.0;
    double percentEquity = 0.01;
    double leverage = 1.0;
    size_t minTrades = 30;
    size_t numThreads = std::thread::hardware_concurrency();
    double capital = 10000.0;
    double feeRate = 0.0004;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--sizing-mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "all_equity") sizingMode = SizingMode::ALL_EQUITY;
            else if (mode == "fixed_notional") sizingMode = SizingMode::FIXED_NOTIONAL;
            else if (mode == "percent_equity") sizingMode = SizingMode::PERCENT_EQUITY;
            else { std::cerr << "Unknown sizing mode: " << mode << "\n"; return 1; }
        }
        else if (arg == "--notional" && i + 1 < argc) { notional = std::stod(argv[++i]); }
        else if (arg == "--percent-equity" && i + 1 < argc) { percentEquity = std::stod(argv[++i]); }
        else if (arg == "--leverage" && i + 1 < argc) { leverage = std::stod(argv[++i]); }
        else if (arg == "--min-trades" && i + 1 < argc) { minTrades = std::stoul(argv[++i]); }
        else if (arg == "--threads" && i + 1 < argc) { numThreads = std::stoul(argv[++i]); }
        else if (arg == "--capital" && i + 1 < argc) { capital = std::stod(argv[++i]); }
        else if (arg == "--fee" && i + 1 < argc) { feeRate = std::stod(argv[++i]); }
        else { numThreads = std::stoul(arg); }  // backward compat: positional thread count
    }
    if (numThreads == 0) numThreads = 1;

    // Extract coin name from filename (e.g., data/BTCUSDT_4h.csv -> BTCUSDT)
    std::string coin = std::filesystem::path(csvPath).stem().string();
    auto usdPos = coin.find("USDT");
    if (usdPos != std::string::npos) coin = coin.substr(0, usdPos + 4);
    else if (coin.find('_') != std::string::npos) coin = coin.substr(0, coin.find('_'));

    // Set global sizing params for all strategies
    g_sizingMode = sizingMode;
    g_notionalUsd = notional;
    g_percentEquity = percentEquity;
    g_leverage = leverage;
    g_capitalForSizing = capital;

    // Backtest config
    g_initialCapital = capital;
    g_feeRate = feeRate;

    std::cerr << "Loading " << csvPath << "...\n";
    auto csvBars = readCsvBars(csvPath);
    if (csvBars.empty()) { std::cerr << "Error: no bars loaded\n"; return 1; }

    // Compute bar period from data
    size_t numBars = csvBars.size();
    double firstPrice = numBars > 0 ? csvBars[0].close : 0.0;
    double lastPrice = numBars > 0 ? csvBars[numBars - 1].close : 0.0;
    int64_t firstNs = numBars > 0 ? csvBars[0].timestampNs : 0;
    int64_t lastNs = numBars > 0 ? csvBars[numBars - 1].timestampNs : 0;
    double barHours = numBars > 1 ? static_cast<double>(lastNs - firstNs) / 1e9 / 3600.0 / static_cast<double>(numBars - 1) : 0.0;

    // Compute effective notional for display
    double effectiveNotional;
    const char* modeStr;
    switch (sizingMode) {
    case SizingMode::ALL_EQUITY:    effectiveNotional = capital * leverage; modeStr = "all_equity"; break;
    case SizingMode::FIXED_NOTIONAL: effectiveNotional = notional * leverage; modeStr = "fixed_notional"; break;
    case SizingMode::PERCENT_EQUITY: effectiveNotional = capital * percentEquity * leverage; modeStr = "percent_equity"; break;
    default: effectiveNotional = notional; modeStr = "fixed_notional";
    }

    std::cerr << "Loaded " << numBars << " bars (" << coin << ")\n";
    std::cerr << "  Period: " << barHours << "h bars | First: $" << firstPrice << " | Last: $" << lastPrice << "\n";
    std::cerr << "  Sizing: " << modeStr << " | Notional: $" << effectiveNotional
              << " | Leverage: " << leverage << "x\n";
    std::cerr << "  Qty per trade: $" << effectiveNotional << " / $" << lastPrice << " = "
              << std::fixed << std::setprecision(6) << effectiveNotional / lastPrice << " " << coin << "\n";

    SymbolInfo info;
    info.id = g_symbolId;
    info.exchange = "BACKTEST";
    info.symbol = coin;
    info.tickSize = Price::fromDouble(0.01);
    g_registry.registerSymbol(info);

    g_bars = csvBarsToBarEvents(csvBars, g_symbolId);

    CombinedGrid grid;
    std::cerr << "\nGrid: " << grid.totalCombinations() << " combinations across " << NUM_STRATEGIES << " strategies\n";
    std::cerr << "Config: sizing=" << modeStr << " | notional=$" << effectiveNotional << " | capital=$" << capital
              << " | fee=" << (feeRate * 100) << "% | min-trades=" << minTrades
              << " | " << numThreads << " threads\n\n";

    auto results = runParallel(grid, numThreads);

    // Global ranking
    auto ranked = BacktestOptimizer<CombinedParams, CombinedGrid>::rankResults(results, RankMetric::SharpeRatio);

    // Filter by min-trades BEFORE display and export
    std::vector<OptimizationResult<CombinedParams>> filtered;
    for (const auto& r : ranked) {
        if (r.totalTrades() >= minTrades) {
            filtered.push_back(r);
        }
    }

    std::cout << "\n=== Top 30 Results (" << coin << " | " << modeStr << " $" << effectiveNotional
              << " | min " << minTrades << " trades | " << filtered.size() << " passed filter) ===\n";
    int shown = 0;
    for (size_t i = 0; i < filtered.size() && shown < 30; ++i)
    {
        std::cout << shown + 1 << ". " << filtered[i].parameters.toString() << "\n"
                  << "   Sharpe: " << std::fixed << std::setprecision(2) << filtered[i].sharpeRatio()
                  << " | Return: " << std::setprecision(1) << filtered[i].totalReturn() << "%"
                  << " | MaxDD: " << filtered[i].maxDrawdownPct() << "%"
                  << " | Trades: " << filtered[i].totalTrades()
                  << " | Win%: " << std::setprecision(1) << (filtered[i].winRate() * 100) << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  (no strategies with >= " << minTrades << " trades)\n";

    // Per-strategy breakdown (uses all results, not just filtered)
    printPerStrategySummary(results);

    // Statistical summary
    using Stats = OptimizationStatistics<CombinedParams, CombinedGrid>;
    Stats::printSummary(results);

    // Export filtered results to results/ folder
    std::filesystem::create_directories("results");
    std::string csvOut = "results/" + coin + "_grid_results.csv";
    BacktestOptimizer<CombinedParams, CombinedGrid>::exportToCSV(filtered, csvOut);
    std::cerr << "\nExported " << filtered.size() << " results (>= " << minTrades << " trades) to " << csvOut << "\n";

    return 0;
}
