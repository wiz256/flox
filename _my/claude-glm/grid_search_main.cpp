/*
 * Grid Search Main — 13 strategies × 1594 combos × robustness validation
 *
 * Pipeline: Grid search → Min-trades filter → Neighborhood plateau → Walk-forward
 *
 * Build:  ./build.sh
 * Usage:  ./build/crush_grid data/BTCUSDT_4h.csv [options]
 */

#include "strategies/strategies.h"
#include "csv_bar_reader.h"

#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/optimization_stats.h"
#include "flox/engine/symbol_registry.h"
#include "flox/position/position_tracker.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
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

inline const char* strategyExitMode(StrategyKind k)
{
    static const char* modes[] = {
        "bb_reverse",          // BOLLINGER_BREAKOUT: exit on BB opposite band touch
        "channel_midline",     // DONCHIAN_BREAKOUT: exit on exit-channel midline cross
        "momentum_flip",       // DUAL_MOMENTUM: exit on momentum sign flip
        "ema_cross",           // EMA_CROSSOVER: exit on EMA cross back
        "channel_break",       // KELTNER_BREAKOUT: exit on Keltner channel break back
        "squeeze_release",     // KELTNER_SQUEEZE: exit on squeeze release signal
        "signal_cross",        // MACD: exit on MACD signal cross
        "bb_mean_revert",      // RSI_BB_MR: exit on RSI exit zone or BB mean
        "rsi_exit_zone",       // RSI2: exit on RSI crossing exit threshold
        "trend_flip",          // SUPERTREND: exit on supertrend direction flip
        "chandelier_trail",    // TREND_PULLBACK: chandelier trailing stop (3xATR)
        "sign_flip",           // TSMOM: exit on return sign flip
        "range_break"          // VOL_COMPRESSION_BREAKOUT: exit on range break reversal
    };
    return modes[k];
}

// Format Unix nanoseconds to YYYY-MM-DD
inline std::string formatDateNs(int64_t timestampNs)
{
    time_t secs = static_cast<time_t>(timestampNs / 1000000000LL);
    struct tm tm_buf;
    gmtime_r(&secs, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
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
            ss << "bb_period=" << ip[0] << ",bb_std=" << dp[0] << ",vol_mult=" << dp[1]; break;
        case DONCHIAN_BREAKOUT:
            ss << "channel=" << ip[0] << ",exit=" << ip[1] << ",vol_mult=" << dp[0]; break;
        case DUAL_MOMENTUM:
            ss << "lookback=" << ip[0] << ",mom_threshold=" << dp[0] << ",smooth=" << ip[1]; break;
        case EMA_CROSSOVER:
            ss << "fast=" << ip[0] << ",slow=" << ip[1] << ",adx_min=" << ip[2]; break;
        case KELTNER_BREAKOUT:
            ss << "ema=" << ip[0] << ",atr=" << ip[1] << ",atr_mult=" << dp[0] << ",atr_pct_min=" << dp[1]; break;
        case KELTNER_SQUEEZE:
            ss << "ema=" << ip[0] << ",kelt_mult=" << dp[0] << ",bb=" << ip[1] << ",bb_std=" << dp[1]; break;
        case MACD:
            ss << "fast=" << ip[0] << ",slow=" << ip[1] << ",signal=" << ip[2] << ",trend=" << ip[3]; break;
        case RSI_BB_MR:
            ss << "rsi_period=" << ip[0] << ",rsi_low=" << dp[0] << ",rsi_high=" << dp[1] << ",bb_period=" << ip[1] << ",bb_std=" << dp[2]; break;
        case RSI2:
            ss << "rsi_period=" << ip[0] << ",entry_low=" << dp[0] << ",entry_high=" << dp[1] << ",trend=" << ip[1]; break;
        case SUPERTREND:
            ss << "atr_period=" << ip[0] << ",atr_mult=" << dp[0] << ",adx_min=" << ip[1]; break;
        case TREND_PULLBACK:
            ss << "fast_ema=" << ip[0] << ",slow_ema=" << ip[1] << ",rsi_pullback=" << ip[2] << ",atr_pct_min=" << dp[0]; break;
        case TSMOM:
            ss << "lookback=" << ip[0] << ",smooth=" << ip[1] << ",atr_pct_min=" << dp[0]; break;
        case VOL_COMPRESSION_BREAKOUT:
            ss << "range_window=" << ip[0] << ",compression_window=" << ip[1] << ",compression_pct=" << dp[0] << ",vol_mult=" << dp[1]; break;
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
// Axis definitions for neighborhood / plateau detection
// =============================================================================

struct AxisDef
{
    enum Type { INT, DOUBLE } type;
    int ipSlot;                      // which ip[] slot (-1 if unused)
    int dpSlot;                      // which dp[] slot (-1 if unused)
    std::vector<int> intVals;
    std::vector<double> dblVals;

    size_t size() const { return type == INT ? intVals.size() : dblVals.size(); }

    // Find index of value in axis. Returns -1 if not found.
    int indexInAxis(const CombinedParams& p) const
    {
        if (type == INT) {
            int v = p.ip[ipSlot];
            for (size_t i = 0; i < intVals.size(); ++i)
                if (intVals[i] == v) return static_cast<int>(i);
        } else {
            double v = p.dp[dpSlot];
            for (size_t i = 0; i < dblVals.size(); ++i)
                if (dblVals[i] == v) return static_cast<int>(i);
        }
        return -1;
    }
};

struct StrategyAxisDefs
{
    std::vector<AxisDef> axes;
};

// Per-strategy axis definitions — extracted from CombinedGrid constructor
static const StrategyAxisDefs g_axisDefs[NUM_STRATEGIES] = {
    /* BOLLINGER_BREAKOUT */ {
        { AxisDef{AxisDef::INT, 0,-1, {10,20,30,50}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {1.5,2.0,2.5,3.0}},
          AxisDef{AxisDef::DOUBLE,-1,1, {}, {0.0,1.3,1.5}} }
    },
    /* DONCHIAN_BREAKOUT */ {
        { AxisDef{AxisDef::INT, 0,-1, {10,15,20,30,40,55}, {}},
          AxisDef{AxisDef::INT, 1,-1, {5,8,10,15,20}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {0.0,1.3,1.5,2.0}} }
    },
    /* DUAL_MOMENTUM */ {
        { AxisDef{AxisDef::INT, 0,-1, {20,40,60,120}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {0.0,0.02,0.05}},
          AxisDef{AxisDef::INT, 1,-1, {1,3,5}, {}} }
    },
    /* EMA_CROSSOVER */ {
        { AxisDef{AxisDef::INT, 0,-1, {5,8,13,21,34}, {}},
          AxisDef{AxisDef::INT, 1,-1, {21,34,55,89,144}, {}},
          AxisDef{AxisDef::INT, 2,-1, {0,15,20,25}, {}} }
    },
    /* KELTNER_BREAKOUT */ {
        { AxisDef{AxisDef::INT, 0,-1, {10,20,30,40,50,60,80}, {}},
          AxisDef{AxisDef::INT, 1,-1, {10,14,20,30}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {2.0,2.5,2.8,3.0,3.2,3.5}},
          AxisDef{AxisDef::DOUBLE,-1,1, {}, {0.0,0.005,0.008}} }
    },
    /* KELTNER_SQUEEZE */ {
        { AxisDef{AxisDef::INT, 0,-1, {10,20,30}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {1.5,2.0,2.5}},
          AxisDef{AxisDef::INT, 1,-1, {10,20,30}, {}},
          AxisDef{AxisDef::DOUBLE,-1,1, {}, {1.5,2.0,2.5}} }
    },
    /* MACD */ {
        { AxisDef{AxisDef::INT, 0,-1, {8,12,16}, {}},
          AxisDef{AxisDef::INT, 1,-1, {21,26,34}, {}},
          AxisDef{AxisDef::INT, 2,-1, {7,9,12}, {}},
          AxisDef{AxisDef::INT, 3,-1, {0,100,200}, {}} }
    },
    /* RSI_BB_MR */ {
        { AxisDef{AxisDef::INT, 0,-1, {7,14,21}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {20.0,25.0,30.0}},
          AxisDef{AxisDef::DOUBLE,-1,1, {}, {70.0,75.0,80.0}},
          AxisDef{AxisDef::INT, 1,-1, {15,20,30}, {}},
          AxisDef{AxisDef::DOUBLE,-1,2, {}, {1.5,2.0,2.5}} }
    },
    /* RSI2 */ {
        { AxisDef{AxisDef::INT, 0,-1, {2,3}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {5.0,10.0,15.0}},
          AxisDef{AxisDef::DOUBLE,-1,1, {}, {85.0,90.0,95.0}},
          AxisDef{AxisDef::INT, 1,-1, {50,100,200}, {}} }
    },
    /* SUPERTREND */ {
        { AxisDef{AxisDef::INT, 0,-1, {7,10,14,20}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {2.0,2.5,3.0,3.5,4.0}},
          AxisDef{AxisDef::INT, 1,-1, {0,20,25}, {}} }
    },
    /* TREND_PULLBACK */ {
        { AxisDef{AxisDef::INT, 0,-1, {10,20,30}, {}},
          AxisDef{AxisDef::INT, 1,-1, {50,100,150,200}, {}},
          AxisDef{AxisDef::INT, 2,-1, {35,40,45}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {0.0,0.004,0.008}} }
    },
    /* TSMOM */ {
        { AxisDef{AxisDef::INT, 0,-1, {20,40,60,80,120,160}, {}},
          AxisDef{AxisDef::INT, 1,-1, {1,3,5,10}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {0.0,0.005,0.008}} }
    },
    /* VOL_COMPRESSION_BREAKOUT */ {
        { AxisDef{AxisDef::INT, 0,-1, {20,30,40,60}, {}},
          AxisDef{AxisDef::INT, 1,-1, {60,100,150}, {}},
          AxisDef{AxisDef::DOUBLE,-1,0, {}, {0.2,0.3,0.4}},
          AxisDef{AxisDef::DOUBLE,-1,1, {}, {0.0,1.3,1.5}} }
    },
};

// =============================================================================
// Neighbor map — find ±1 param step neighbors for plateau detection
// =============================================================================

struct NeighborInfo
{
    double plateauRatio = 0.0;      // fraction of profitable neighbors
    double avgNeighborSharpe = 0.0;
    int    neighborCount = 0;
    int    profitableCount = 0;
};

class NeighborMap
{
public:
    void build(const CombinedGrid& grid)
    {
        // Build param-string → index lookup
        _paramIndex.clear();
        for (size_t i = 0; i < grid.entries.size(); ++i)
            _paramIndex[grid.entries[i].toString()] = i;
    }

    // Find all neighbors (±1 step in exactly one axis) for grid entry at idx
    std::vector<size_t> findNeighbors(const CombinedParams& p) const
    {
        std::vector<size_t> neighbors;
        int k = static_cast<int>(p.kind);
        if (k < 0 || k >= NUM_STRATEGIES) return neighbors;

        const auto& axes = g_axisDefs[k].axes;
        for (const auto& axis : axes)
        {
            int curIdx = axis.indexInAxis(p);
            if (curIdx < 0) continue;

            for (int delta : {-1, +1})
            {
                int newIdx = curIdx + delta;
                if (newIdx < 0 || static_cast<size_t>(newIdx) >= axis.size()) continue;

                // Create neighbor params
                CombinedParams nb = p;
                if (axis.type == AxisDef::INT)
                    nb.ip[axis.ipSlot] = axis.intVals[newIdx];
                else
                    nb.dp[axis.dpSlot] = axis.dblVals[newIdx];

                // Look up in the grid
                auto it = _paramIndex.find(nb.toString());
                if (it != _paramIndex.end())
                    neighbors.push_back(it->second);
            }
        }
        return neighbors;
    }

    // Compute plateau info for all grid entries
    std::vector<NeighborInfo> computePlateau(
        const CombinedGrid& grid,
        const std::vector<OptimizationResult<CombinedParams>>& results,
        size_t minTrades) const
    {
        std::vector<NeighborInfo> info(grid.totalCombinations());

        for (size_t i = 0; i < grid.totalCombinations(); ++i)
        {
            auto nbs = findNeighbors(grid[i]);
            double sharpeSum = 0.0;
            int profitable = 0;
            int valid = 0;

            for (size_t ni : nbs)
            {
                if (ni >= results.size()) continue;
                // Count neighbor as valid if it has enough trades
                if (results[ni].totalTrades() >= minTrades || results[ni].totalTrades() > 0)
                {
                    valid++;
                    double s = results[ni].sharpeRatio();
                    sharpeSum += s;
                    if (s > 0.0) profitable++;
                }
            }

            info[i].neighborCount = valid;
            info[i].profitableCount = profitable;
            info[i].plateauRatio = valid > 0 ? static_cast<double>(profitable) / static_cast<double>(valid) : 0.0;
            info[i].avgNeighborSharpe = valid > 0 ? sharpeSum / static_cast<double>(valid) : 0.0;
        }
        return info;
    }

private:
    std::unordered_map<std::string, size_t> _paramIndex;
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

// Run backtest on a slice of bars (for walk-forward)
BacktestResult runBacktestOnSlice(const CombinedParams& p, const std::vector<BarEvent>& bars)
{
    BacktestConfig config;
    config.initialCapital = g_initialCapital;
    config.feeRate = g_feeRate;

    BacktestRunner runner(config);
    Strategy* strat = createStrategy(p);
    runner.setStrategy(strat);
    BacktestResult result = runner.runBars(bars);
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
// Walk-forward validation — rolling WFO with embargo (matches Trader7 config)
// Uses BacktestRunner::runBars with full OHLCV (not FLOX's WalkForwardRunner)
// =============================================================================

struct WfoFoldResult
{
    size_t foldIndex;
    size_t trainStart, trainEnd;
    size_t testStart, testEnd;
    size_t trainBars;         // bars in IS window
    size_t testBars;          // bars in OOS window
    size_t embargoStart;      // where embargo begins
    std::string trainDateFrom, trainDateTo;
    std::string testDateFrom, testDateTo;
    double trainPriceStart, trainPriceEnd;  // price at IS window boundaries
    double testPriceStart, testPriceEnd;    // price at OOS window boundaries
    BacktestStats trainStats;
    BacktestStats testStats;
    double degradation;   // testSharpe / trainSharpe
    bool skipped;         // skipped due to insufficient trades
    bool passed;
    std::string skipReason;  // why fold was skipped (empty if not skipped)
};

// Rolling WFO: fixed-size train window slides forward, test window follows with embargo gap.
// Matches Trader7 quant_scout-7 config: 70% IS, 30% OOS, 24-bar embargo, per-fold trade gating.
std::vector<WfoFoldResult> runWalkForward(
    const CombinedParams& p,
    size_t numFolds,
    double isPct,           // fraction of fold period for in-sample (default 0.70)
    size_t embargoBars,     // gap between train end and test start (default 24)
    size_t minIsTrades,     // minimum trades in IS fold (default 50)
    size_t minOosTrades,    // minimum trades in OOS fold (default 10)
    double oosVsIsRatio,    // minimum OOS/IS Sharpe ratio (default 0.60)
    double foldPassPct)     // fraction of folds that must pass (default 0.60)
{
    size_t totalBars = g_bars.size();

    // Compute fold period: divide total bars into N folds
    size_t foldPeriod = totalBars / numFolds;
    if (foldPeriod < embargoBars + 100) return {};  // need enough bars per fold

    size_t trainSize = static_cast<size_t>(static_cast<double>(foldPeriod) * isPct);
    size_t testSize = foldPeriod - trainSize - embargoBars;
    if (trainSize < 50 || testSize < 20) return {};

    std::vector<WfoFoldResult> folds;

    for (size_t fold = 0; fold < numFolds; ++fold)
    {
        size_t foldStart = fold * foldPeriod;
        size_t trainStart = foldStart;
        size_t trainEnd = foldStart + trainSize;
        size_t embargoStart = trainEnd;
        size_t testStart = trainEnd + embargoBars;  // embargo gap
        size_t testEnd = std::min(testStart + testSize, totalBars);

        if (testEnd <= testStart || trainEnd > totalBars) break;

        // Slice bars: rolling window (not anchored)
        std::vector<BarEvent> trainBars(g_bars.begin() + trainStart, g_bars.begin() + trainEnd);
        std::vector<BarEvent> testBars(g_bars.begin() + testStart, g_bars.begin() + testEnd);

        // Extract dates and prices from bar boundaries
        // Bars have endTime as their timestamp; use bar.close for prices
        double trPriceStart = trainBars.empty() ? 0.0 : trainBars.front().bar.close.toDouble();
        double trPriceEnd   = trainBars.empty() ? 0.0 : trainBars.back().bar.close.toDouble();
        double tePriceStart = testBars.empty() ? 0.0 : testBars.front().bar.close.toDouble();
        double tePriceEnd   = testBars.empty() ? 0.0 : testBars.back().bar.close.toDouble();

        // Dates from bar endTime nanoseconds
        std::string trDateFrom = trainBars.empty() ? "" : formatDateNs(trainBars.front().bar.endTime.time_since_epoch().count());
        std::string trDateTo   = trainBars.empty() ? "" : formatDateNs(trainBars.back().bar.endTime.time_since_epoch().count());
        std::string teDateFrom = testBars.empty() ? "" : formatDateNs(testBars.front().bar.endTime.time_since_epoch().count());
        std::string teDateTo   = testBars.empty() ? "" : formatDateNs(testBars.back().bar.endTime.time_since_epoch().count());

        BacktestResult trainBt = runBacktestOnSlice(p, trainBars);
        auto trainStats = trainBt.computeStats();

        BacktestResult testBt = runBacktestOnSlice(p, testBars);
        auto testStats = testBt.computeStats();

        // Determine skip reason
        std::string skipReason;
        bool skipped = false;
        if (trainStats.totalTrades < minIsTrades) {
            skipped = true;
            skipReason = "IS trades=" + std::to_string(trainStats.totalTrades) + " < " + std::to_string(minIsTrades);
        } else if (testStats.totalTrades < minOosTrades) {
            skipped = true;
            skipReason = "OOS trades=" + std::to_string(testStats.totalTrades) + " < " + std::to_string(minOosTrades);
        }

        double degradation = trainStats.sharpeRatio > 0.0
            ? testStats.sharpeRatio / trainStats.sharpeRatio
            : 0.0;

        // Per-fold pass: OOS Sharpe > 0, OOS/IS ratio >= threshold, enough trades
        bool passed = !skipped
            && testStats.sharpeRatio > 0.0
            && degradation >= oosVsIsRatio;

        folds.push_back({fold, trainStart, trainEnd, testStart, testEnd,
                         trainBars.size(), testBars.size(), embargoStart,
                         trDateFrom, trDateTo, teDateFrom, teDateTo,
                         trPriceStart, trPriceEnd, tePriceStart, tePriceEnd,
                         trainStats, testStats, degradation, skipped, passed, skipReason});
    }

    return folds;
}

// Evaluate overall WFO pass/fail for a set of folds
// Returns pass rate (0-1). Overall pass if passRate >= foldPassPct.
double wfoPassRate(const std::vector<WfoFoldResult>& folds, double foldPassPct)
{
    if (folds.empty()) return 0.0;
    size_t nonSkipped = 0, passed = 0;
    for (const auto& f : folds)
    {
        if (!f.skipped) { nonSkipped++; if (f.passed) passed++; }
    }
    return nonSkipped > 0 ? static_cast<double>(passed) / static_cast<double>(nonSkipped) : 0.0;
}

// =============================================================================
// Enhanced result row (includes plateau + WFO)
// =============================================================================

struct CompositeComponents
{
    double sharpe = 0, calmar = 0, profitFactor = 0, plateau = 0;
    double trades = 0, costStress = 0, winRate = 0, ddPenalty = 0;
};

struct RobustResult
{
    CombinedParams params;
    const char* exitMode;      // strategy exit mode description
    double sharpe;
    double sortino;
    double calmar;
    double totalReturn;
    double maxDrawdownPct;
    double winRate;
    double profitFactor;
    size_t totalTrades;
    double plateauRatio;
    double avgNeighborSharpe;
    int neighborCount;
    double compositeScore;   // Trader7-style composite
    CompositeComponents cc;  // score breakdown
    double wfoPassRate;      // -1 if WFO not run
    double wfoAvgTestSharpe;
};

// Trader7-style composite score: Sharpe is only 25% of the total.
// Prioritizes stable plateaus over high-Sharpe spikes.
// Fills CompositeComponents with each weighted sub-score for reporting.
inline double computeCompositeScore(double sharpe, double calmar, double profitFactor,
                                     double plateauRatio, double winRate, size_t trades,
                                     double maxDrawdownPct, double costStressSharpe,
                                     CompositeComponents& cc)
{
    // Normalize each component using tanh to bound [0, 1]
    auto norm = [](double v, double scale) { return std::tanh(std::max(v, 0.0) / scale); };

    double nSharpe     = norm(sharpe, 3.0);       // tanh(sharpe/3): 3.0 → 0.76, 6.0 → 0.96
    double safeCalmar  = std::isnan(calmar) || std::isinf(calmar) ? 0.0 : calmar;
    double nCalmar     = norm(safeCalmar, 10.0);   // Calmar = return/maxDD
    double safePF      = std::isnan(profitFactor) || std::isinf(profitFactor) ? 0.0 : profitFactor;
    double nPF         = norm(safePF - 1.0, 1.5); // tanh((pf-1)/1.5): 2.5 → 0.76
    double nPlateau    = std::min(std::max(plateauRatio, 0.0), 1.0);
    double nTrades     = norm(static_cast<double>(trades), 150.0); // tanh(trades/150)
    double nCost       = norm(costStressSharpe, 2.0);  // Sharpe after 2x fees
    double nWinRate    = std::min(std::max(winRate - 0.35, 0.0) / 0.45, 1.0); // 35-80% → 0-1

    // Drawdown penalty (like Trader7: max(0, dd - 35%))
    double ddPenalty = std::max(0.0, std::abs(maxDrawdownPct) - 35.0) / 100.0;

    // Composite: 25% sharpe + 20% calmar + 15% PF + 15% plateau + 10% trades + 10% cost + 5% winrate - penalties
    double score = 0.25 * nSharpe
                 + 0.20 * nCalmar
                 + 0.15 * nPF
                 + 0.15 * nPlateau
                 + 0.10 * nTrades
                 + 0.10 * nCost
                 + 0.05 * nWinRate
                 - ddPenalty;

    // Fill component breakdown (weighted values)
    cc.sharpe      = 0.25 * nSharpe;
    cc.calmar      = 0.20 * nCalmar;
    cc.profitFactor = 0.15 * nPF;
    cc.plateau     = 0.15 * nPlateau;
    cc.trades      = 0.10 * nTrades;
    cc.costStress  = 0.10 * nCost;
    cc.winRate     = 0.05 * nWinRate;
    cc.ddPenalty   = ddPenalty;

    return score;
}

// =============================================================================
// Per-strategy summary
// =============================================================================

void printPerStrategySummary(const std::vector<OptimizationResult<CombinedParams>>& results,
                              const std::vector<NeighborInfo>& plateau)
{
    struct StratStats {
        size_t count = 0, profitable = 0;
        double bestSharpe = -999, bestRobust = -999;
        double avgSharpe = 0, avgPlateau = 0;
    };
    StratStats stats[NUM_STRATEGIES];

    for (size_t i = 0; i < results.size(); ++i)
    {
        int k = static_cast<int>(results[i].parameters.kind);
        if (k < 0 || k >= NUM_STRATEGIES) continue;
        auto& s = stats[k];
        s.count++;
        double sh = results[i].sharpeRatio();
        s.avgSharpe += sh;
        s.avgPlateau += plateau[i].plateauRatio;
        if (sh > 0) s.profitable++;
        double robust = sh * plateau[i].plateauRatio;
        if (robust > s.bestRobust) {
            s.bestRobust = robust;
            s.bestSharpe = sh;
        }
    }

    std::cout << "\n=== Per-Strategy Summary ===\n";
    std::cout << "Strategy                   | Combos | Profit% | Avg Plateau | Best Sharpe | Best Robust\n";
    std::cout << "---------------------------|--------|---------|-------------|-------------|------------\n";
    for (int k = 0; k < NUM_STRATEGIES; ++k)
    {
        auto& s = stats[k];
        if (s.count == 0) continue;
        double profitPct = static_cast<double>(s.profitable) / static_cast<double>(s.count) * 100.0;
        double avgS = s.avgSharpe / static_cast<double>(s.count);
        double avgP = s.avgPlateau / static_cast<double>(s.count);
        printf("%-26s | %5zu  | %5.1f%%  | %11.2f | %11.2f | %10.2f\n",
               strategyName(static_cast<StrategyKind>(k)), s.count, profitPct, avgP, s.bestSharpe, s.bestRobust);
    }
}

// =============================================================================
// Custom CSV export with robustness columns
// =============================================================================

void exportRobustCSV(const std::string& path, const std::vector<RobustResult>& rows,
                     const std::string& timeframe, const std::string& dateFrom,
                     const std::string& dateTo, size_t barsCount,
                     double priceStart, double priceEnd)
{
    std::ofstream f(path);
    if (!f.is_open()) { std::cerr << "Error: cannot write " << path << "\n"; return; }

    f << "timeframe,date_from,date_to,bars_count,price_start,price_end,"
      << "sharpe_ratio,sortino_ratio,calmar_ratio,total_return,max_drawdown_pct,win_rate,"
      << "profit_factor,total_trades,exit_mode,parameters,plateau_ratio,avg_neighbor_sharpe,neighbor_count,composite_score,"
      << "cc_sharpe,cc_calmar,cc_profit_factor,cc_plateau,cc_trades,cc_cost_stress,cc_win_rate,cc_dd_penalty,"
      << "wfo_pass_rate,wfo_avg_test_sharpe\n";

    f << std::fixed << std::setprecision(4);
    for (const auto& r : rows)
    {
        f << timeframe << "," << dateFrom << "," << dateTo << "," << barsCount << ","
          << priceStart << "," << priceEnd << ","
          << r.sharpe << "," << r.sortino << "," << r.calmar << ","
          << r.totalReturn << "," << r.maxDrawdownPct << "," << r.winRate << ","
          << r.profitFactor << "," << r.totalTrades << "," << r.exitMode << ",\"" << r.params.toString() << "\","
          << r.plateauRatio << "," << r.avgNeighborSharpe << ","
          << r.neighborCount << "," << r.compositeScore << ","
          << r.cc.sharpe << "," << r.cc.calmar << "," << r.cc.profitFactor << ","
          << r.cc.plateau << "," << r.cc.trades << "," << r.cc.costStress << ","
          << r.cc.winRate << "," << r.cc.ddPenalty << ","
          << r.wfoPassRate << "," << r.wfoAvgTestSharpe << "\n";
    }
}

void exportWfoCSV(const std::string& path,
                  const std::vector<std::pair<CombinedParams, std::vector<WfoFoldResult>>>& wfoResults)
{
    std::ofstream f(path);
    if (!f.is_open()) { std::cerr << "Error: cannot write " << path << "\n"; return; }

    f << "fold,parameters,"
      << "train_date_from,train_date_to,train_bars,train_price_start,train_price_end,"
      << "train_sharpe,train_sortino,train_calmar,train_return,train_maxdd,train_trades,train_win_rate,train_profit_factor,train_pnl,train_net_pnl,train_fees,"
      << "test_date_from,test_date_to,test_bars,test_price_start,test_price_end,"
      << "test_sharpe,test_sortino,test_calmar,test_return,test_maxdd,test_trades,test_win_rate,test_profit_factor,test_pnl,test_net_pnl,test_fees,"
      << "degradation,passed,skipped,skip_reason\n";

    f << std::fixed << std::setprecision(4);
    for (const auto& [params, folds] : wfoResults)
    {
        for (const auto& fold : folds)
        {
            f << fold.foldIndex << ",\"" << params.toString() << "\","
              << fold.trainDateFrom << "," << fold.trainDateTo << ","
              << fold.trainBars << "," << fold.trainPriceStart << "," << fold.trainPriceEnd << ","
              << fold.trainStats.sharpeRatio << "," << fold.trainStats.sortinoRatio << ","
              << fold.trainStats.calmarRatio << "," << fold.trainStats.returnPct << ","
              << fold.trainStats.maxDrawdownPct << "," << fold.trainStats.totalTrades << ","
              << fold.trainStats.winRate << "," << fold.trainStats.profitFactor << ","
              << fold.trainStats.totalPnl << "," << fold.trainStats.netPnl << ","
              << fold.trainStats.totalFees << ","
              << fold.testDateFrom << "," << fold.testDateTo << ","
              << fold.testBars << "," << fold.testPriceStart << "," << fold.testPriceEnd << ","
              << fold.testStats.sharpeRatio << "," << fold.testStats.sortinoRatio << ","
              << fold.testStats.calmarRatio << "," << fold.testStats.returnPct << ","
              << fold.testStats.maxDrawdownPct << "," << fold.testStats.totalTrades << ","
              << fold.testStats.winRate << "," << fold.testStats.profitFactor << ","
              << fold.testStats.totalPnl << "," << fold.testStats.netPnl << ","
              << fold.testStats.totalFees << ","
              << fold.degradation << "," << (fold.passed ? 1 : 0) << ","
              << (fold.skipped ? 1 : 0) << ",\"" << fold.skipReason << "\"\n";
        }
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
                  << "  --percent-equity <pct>  Equity fraction for percent_equity mode (default: 0.01)\n"
                  << "  --leverage <mult>       Leverage multiplier (default: 1.0)\n"
                  << "  --min-trades <n>        Minimum trades filter (default: 30)\n"
                  << "  --threads <n>           Number of threads (default: all cores)\n"
                  << "  --capital <usd>         Initial capital (default: 10000)\n"
                  << "  --fee <rate>            Fee rate (default: 0.0004)\n"
                  << "  --wfo                   Enable walk-forward validation (default: on)\n"
                  << "  --no-wfo                Disable walk-forward validation\n"
                  << "  --top-k <n>             Top-K candidates for WFO (default: 50)\n"
                  << "  --plateau-min <f>       Min plateau ratio (default: 0.3)\n"
                  << "  --recent-years <n>      Use only last N years of data (default: 0=all)\n"
                  << "  --wfo-folds <n>         WFO folds (default: 5)\n"
                  << "  --wfo-is-pct <f>        IS fraction per fold (default: 0.70)\n"
                  << "  --wfo-embargo <n>       Embargo bars between IS/OOS (default: 24)\n"
                  << "  --wfo-min-is-trades <n> Min IS trades per fold (default: 20)\n"
                  << "  --wfo-min-oos-trades <n> Min OOS trades per fold (default: 5)\n"
                  << "  --wfo-degrad <f>        Min OOS/IS Sharpe ratio (default: 0.30)\n"
                  << "  --wfo-fold-pass <f>     Fraction of folds that must pass (default: 0.50)\n";
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
    bool wfoEnabled = true;
    size_t topK = 50;
    double plateauMin = 0.3;
    double recentYears = 0.0;  // 0 = use all data
    size_t wfoFolds = 5;
    double wfoIsPct = 0.70;
    size_t wfoEmbargo = 24;
    size_t wfoMinIsTrades = 20;    // 50 too strict for 4h bars; 20 fits ~1680 IS bars
    size_t wfoMinOosTrades = 5;    // 10 too strict for short OOS windows
    double wfoDegradation = 0.30;  // 0.60 too strict; 0.30 allows some degradation
    double wfoFoldPassPct = 0.50;  // 0.60 too strict; need majority not supermajority

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
        else if (arg == "--wfo") { wfoEnabled = true; }
        else if (arg == "--no-wfo") { wfoEnabled = false; }
        else if (arg == "--top-k" && i + 1 < argc) { topK = std::stoul(argv[++i]); }
        else if (arg == "--plateau-min" && i + 1 < argc) { plateauMin = std::stod(argv[++i]); }
        else if (arg == "--recent-years" && i + 1 < argc) { recentYears = std::stod(argv[++i]); }
        else if (arg == "--wfo-folds" && i + 1 < argc) { wfoFolds = std::stoul(argv[++i]); }
        else if (arg == "--wfo-is-pct" && i + 1 < argc) { wfoIsPct = std::stod(argv[++i]); }
        else if (arg == "--wfo-embargo" && i + 1 < argc) { wfoEmbargo = std::stoul(argv[++i]); }
        else if (arg == "--wfo-min-is-trades" && i + 1 < argc) { wfoMinIsTrades = std::stoul(argv[++i]); }
        else if (arg == "--wfo-min-oos-trades" && i + 1 < argc) { wfoMinOosTrades = std::stoul(argv[++i]); }
        else if (arg == "--wfo-degrad" && i + 1 < argc) { wfoDegradation = std::stod(argv[++i]); }
        else if (arg == "--wfo-fold-pass" && i + 1 < argc) { wfoFoldPassPct = std::stod(argv[++i]); }
        else { numThreads = std::stoul(arg); }  // backward compat: positional thread count
    }
    if (numThreads == 0) numThreads = 1;

    // Extract coin name and timeframe from filename (e.g., "BTCUSDT_4h" → coin="BTCUSDT", timeframe="4h")
    std::string coin = fs::path(csvPath).stem().string();
    std::string timeframe = "unknown";
    auto usdPos = coin.find("USDT");
    if (usdPos != std::string::npos) coin = coin.substr(0, usdPos + 4);
    else if (coin.find('_') != std::string::npos) coin = coin.substr(0, coin.find('_'));
    // Extract timeframe from original filename
    {
        std::string stem = fs::path(csvPath).stem().string();
        auto lastUnderscore = stem.rfind('_');
        if (lastUnderscore != std::string::npos && lastUnderscore + 1 < stem.size())
            timeframe = stem.substr(lastUnderscore + 1);
    }

    // Set global sizing params
    g_sizingMode = sizingMode;
    g_notionalUsd = notional;
    g_percentEquity = percentEquity;
    g_leverage = leverage;
    g_capitalForSizing = capital;
    g_initialCapital = capital;
    g_feeRate = feeRate;

    std::cerr << "Loading " << csvPath << "...\n";
    auto csvBars = readCsvBars(csvPath);
    if (csvBars.empty()) { std::cerr << "Error: no bars loaded\n"; return 1; }

    size_t numBars = csvBars.size();
    double firstPrice = csvBars[0].close;
    double lastPrice = csvBars[numBars - 1].close;
    std::string dateFrom = formatDateNs(csvBars[0].timestampNs);
    std::string dateTo = formatDateNs(csvBars[numBars - 1].timestampNs);

    double effectiveNotional;
    const char* modeStr;
    switch (sizingMode) {
    case SizingMode::ALL_EQUITY:    effectiveNotional = capital * leverage; modeStr = "all_equity"; break;
    case SizingMode::FIXED_NOTIONAL: effectiveNotional = notional * leverage; modeStr = "fixed_notional"; break;
    case SizingMode::PERCENT_EQUITY: effectiveNotional = capital * percentEquity * leverage; modeStr = "percent_equity"; break;
    default: effectiveNotional = notional; modeStr = "fixed_notional";
    }

    std::cerr << "Loaded " << numBars << " " << timeframe << " bars (" << coin << ")\n";
    std::cerr << "  Period: " << dateFrom << " → " << dateTo
              << " | $" << firstPrice << " → $" << lastPrice << "\n";
    std::cerr << "  Sizing: " << modeStr << " | Notional: $" << effectiveNotional
              << " | Leverage: " << leverage << "x\n";

    SymbolInfo info;
    info.id = g_symbolId;
    info.exchange = "BACKTEST";
    info.symbol = coin;
    info.tickSize = Price::fromDouble(0.01);
    g_registry.registerSymbol(info);
    g_bars = csvBarsToBarEvents(csvBars, g_symbolId);

    // Truncate to recent years if requested
    if (recentYears > 0.0 && numBars > 0)
    {
        // Estimate bars per year from data span
        double dataYears = static_cast<double>(csvBars[numBars-1].timestampNs - csvBars[0].timestampNs) / 1e9 / (365.25 * 86400.0);
        if (dataYears > recentYears)
        {
            size_t keepBars = static_cast<size_t>(static_cast<double>(numBars) * recentYears / dataYears);
            keepBars = std::min(keepBars, numBars);
            size_t startIdx = numBars - keepBars;
            g_bars.erase(g_bars.begin(), g_bars.begin() + startIdx);
            // Update date range and prices to reflect truncated data
            dateFrom = formatDateNs(csvBars[startIdx].timestampNs);
            firstPrice = csvBars[startIdx].close;
            std::cerr << "  Truncated to last " << recentYears << " years: "
                      << g_bars.size() << " bars (" << dateFrom << " → " << dateTo << ")\n";
        }
    }

    CombinedGrid grid;
    std::cerr << "\nGrid: " << grid.totalCombinations() << " combinations across " << NUM_STRATEGIES << " strategies\n";
    std::cerr << "Config: sizing=" << modeStr << " | notional=$" << effectiveNotional << " | capital=$" << capital
              << " | fee=" << (feeRate * 100) << "% | min-trades=" << minTrades
              << " | " << numThreads << " threads"
              << " | WFO=" << (wfoEnabled ? "ON" : "OFF") << "\n\n";

    // ===== Step 1: Grid search =====
    auto results = runParallel(grid, numThreads);

    // ===== Step 2: Neighborhood / Plateau detection =====
    std::cerr << "Computing neighborhood plateau scores...\n";
    NeighborMap nmap;
    nmap.build(grid);
    auto plateau = nmap.computePlateau(grid, results, minTrades);

    size_t withPlateau = 0;
    for (const auto& p : plateau)
        if (p.plateauRatio >= plateauMin) ++withPlateau;
    std::cerr << "  " << withPlateau << "/" << grid.totalCombinations()
              << " have plateauRatio >= " << plateauMin << "\n";

    // ===== Step 3: Filter + rank by robustScore =====
    // Build robust results, filter by min-trades
    std::vector<RobustResult> robustRows;
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (results[i].totalTrades() < minTrades) continue;

        RobustResult rr;
        rr.params = results[i].parameters;
        rr.exitMode = strategyExitMode(results[i].parameters.kind);
        rr.sharpe = results[i].sharpeRatio();
        rr.sortino = results[i].sortinoRatio();
        rr.calmar = results[i].calmarRatio();
        rr.totalReturn = results[i].totalReturn();
        rr.maxDrawdownPct = results[i].maxDrawdownPct();
        rr.winRate = results[i].winRate();
        rr.profitFactor = results[i].profitFactor();
        rr.totalTrades = results[i].totalTrades();
        rr.plateauRatio = plateau[i].plateauRatio;
        rr.avgNeighborSharpe = plateau[i].avgNeighborSharpe;
        rr.neighborCount = plateau[i].neighborCount;
        // Cost stress: approximate Sharpe after 2x fees (halve excess returns)
        double costStressSharpe = rr.sharpe * 0.5;
        rr.compositeScore = computeCompositeScore(
            rr.sharpe, rr.calmar, rr.profitFactor, rr.plateauRatio,
            rr.winRate, rr.totalTrades, rr.maxDrawdownPct, costStressSharpe,
            rr.cc);
        rr.wfoPassRate = -1.0;
        rr.wfoAvgTestSharpe = 0.0;
        robustRows.push_back(rr);
    }

    // Sort by compositeScore descending (Trader7-style: prioritizes stable plateaus)
    std::sort(robustRows.begin(), robustRows.end(),
              [](const RobustResult& a, const RobustResult& b) {
                  return a.compositeScore > b.compositeScore;
              });

    // ===== Step 4: Walk-forward for top-K =====
    std::vector<std::pair<CombinedParams, std::vector<WfoFoldResult>>> wfoAll;

    // Auto-adaptive fold count: reduce folds when bars-per-fold is too small.
    // With 4h bars: ~2190 bars/year. 5 folds → ~438 bars/fold → ~306 IS + ~108 OOS.
    // For recent-years or short data, we may need fewer folds to get meaningful trade counts.
    if (wfoEnabled)
    {
        size_t totalWfoBars = g_bars.size();
        size_t barsPerFold = totalWfoBars / wfoFolds;
        size_t isBars = static_cast<size_t>(static_cast<double>(barsPerFold) * wfoIsPct);
        size_t oosBars = barsPerFold - isBars - wfoEmbargo;

        // If OOS window < 80 bars (20 days at 4h), or IS window < 200 bars, reduce folds
        while (wfoFolds > 2 && (oosBars < 80 || isBars < 200))
        {
            wfoFolds--;
            barsPerFold = totalWfoBars / wfoFolds;
            isBars = static_cast<size_t>(static_cast<double>(barsPerFold) * wfoIsPct);
            oosBars = barsPerFold - isBars - wfoEmbargo;
        }

        if (wfoFolds < 5)
            std::cerr << "  Auto-adapted WFO folds: " << wfoFolds << " ("
                      << totalWfoBars << " bars → " << isBars << " IS + "
                      << oosBars << " OOS per fold)\n";
    }

    if (wfoEnabled && !robustRows.empty())
    {
        // Only WFO candidates with decent plateau
        std::vector<const RobustResult*> wfoCandidates;
        for (const auto& rr : robustRows)
        {
            if (rr.plateauRatio >= plateauMin && wfoCandidates.size() < topK)
                wfoCandidates.push_back(&rr);
        }

        std::cerr << "\nWalk-forward (rolling): " << wfoCandidates.size() << " candidates"
                  << " | " << wfoFolds << " folds"
                  << " | IS=" << (wfoIsPct * 100) << "%"
                  << " | embargo=" << wfoEmbargo
                  << " | OOS/IS≥" << wfoDegradation
                  << " | fold-pass≥" << wfoFoldPassPct << "\n";

        for (size_t ci = 0; ci < wfoCandidates.size(); ++ci)
        {
            const auto& cand = *wfoCandidates[ci];
            auto folds = runWalkForward(cand.params, wfoFolds,
                                        wfoIsPct, wfoEmbargo,
                                        wfoMinIsTrades, wfoMinOosTrades,
                                        wfoDegradation, wfoFoldPassPct);
            wfoAll.emplace_back(cand.params, folds);

            // Use wfoPassRate() which accounts for skipped folds
            double passRate = wfoPassRate(folds, wfoFoldPassPct);
            double testSharpeSum = 0.0;
            size_t nonSkipped = 0;
            for (const auto& f : folds)
            {
                if (!f.skipped) { testSharpeSum += f.testStats.sharpeRatio; ++nonSkipped; }
            }
            double avgTestSharpe = nonSkipped > 0 ? testSharpeSum / static_cast<double>(nonSkipped) : 0.0;

            // Find and update the robust result
            for (auto& rr : robustRows)
            {
                if (rr.params.toString() == cand.params.toString())
                {
                    rr.wfoPassRate = passRate;
                    rr.wfoAvgTestSharpe = avgTestSharpe;
                    break;
                }
            }

            if ((ci + 1) % 10 == 0 || ci + 1 == wfoCandidates.size())
                std::cerr << "\r  WFO: " << ci + 1 << "/" << wfoCandidates.size() << std::flush;
        }
        std::cerr << "\n";
    }

    // Re-sort by compositeScore (now includes WFO data)
    std::sort(robustRows.begin(), robustRows.end(),
              [](const RobustResult& a, const RobustResult& b) {
                  return a.compositeScore > b.compositeScore;
              });

    // ===== Step 5: Display results =====
    std::cout << "\n=== Top 30 by Composite Score ===\n";
    std::cout << "  Coin: " << coin << " | Timeframe: " << timeframe << " | Bars: " << g_bars.size() << "\n";
    std::cout << "  Period: " << dateFrom << " → " << dateTo
              << " | $" << firstPrice << " → $" << lastPrice << "\n";
    std::cout << "  Sizing: " << modeStr << " $" << effectiveNotional << " | Leverage: " << leverage
              << "x | Fee: " << (feeRate * 100) << "%\n";
    std::cout << "  Filter: min " << minTrades << " trades | " << robustRows.size() << " passed\n";
    std::cout << "  WFO: " << (wfoEnabled ? "ON" : "OFF");
    if (wfoEnabled) std::cout << " | " << wfoFolds << " folds | IS=" << (wfoIsPct*100) << "% | embargo=" << wfoEmbargo;
    std::cout << "\n\n";

    std::cout << "   Sharpe | Calmar | PF   | Trades | Plateau | Composite | WFO% | Exit            | Params\n";
    std::cout << "  --------|--------|------|--------|---------|-----------|------|-----------------|-------\n";

    int shown = 0;
    for (size_t i = 0; i < robustRows.size() && shown < 30; ++i)
    {
        const auto& r = robustRows[i];
        std::cout << std::fixed << std::setprecision(2);
        std::cout << shown + 1 << ". "
                  << std::setw(7) << r.sharpe << " | "
                  << std::setw(6) << r.calmar << " | "
                  << std::setw(4) << std::setprecision(2) << r.profitFactor << " | "
                  << std::setw(6) << r.totalTrades << " | "
                  << std::setw(7) << r.plateauRatio << " | "
                  << std::setw(9) << r.compositeScore << " | ";
        if (r.wfoPassRate >= 0)
            std::cout << std::setw(3) << std::setprecision(0) << (r.wfoPassRate * 100) << "%";
        else
            std::cout << " n/a";
        std::cout << " | " << std::left << std::setw(15) << r.exitMode << std::right
                  << " | " << r.params.toString() << "\n";

        // Show composite component breakdown
        std::cout << "     Score breakdown: sharpe=" << std::setprecision(3) << r.cc.sharpe
                  << " calmar=" << r.cc.calmar
                  << " profit_factor=" << r.cc.profitFactor
                  << " plateau=" << r.cc.plateau
                  << " trades=" << r.cc.trades
                  << " cost=" << r.cc.costStress
                  << " win_rate=" << r.cc.winRate
                  << " dd_penalty=" << r.cc.ddPenalty << "\n";
        ++shown;
    }
    if (shown == 0) std::cout << "  (no strategies with >= " << minTrades << " trades)\n";

    // ===== WFO Fold Detail — show per-fold diagnostics for top 10 candidates =====
    if (wfoEnabled && !wfoAll.empty())
    {
        // Build a lookup from params string → folds
        std::unordered_map<std::string, const std::vector<WfoFoldResult>*> wfoLookup;
        for (const auto& [params, folds] : wfoAll)
            wfoLookup[params.toString()] = &folds;

        int wfoShown = 0;
        for (size_t i = 0; i < robustRows.size() && wfoShown < 10; ++i)
        {
            const auto& r = robustRows[i];
            auto it = wfoLookup.find(r.params.toString());
            if (it == wfoLookup.end()) continue;

            const auto& folds = *it->second;
            if (folds.empty()) continue;

            std::cout << "\n--- WFO Detail #" << wfoShown + 1 << ": " << r.params.toString() << " ---\n";
            std::cout << "  Fold | IS Period              | Bars | Price          | Sharpe | Return% | Trades | PnL      | Win%  | PF   | MaxDD%  | Fees\n";
            std::cout << "  -----|------------------------|------|----------------|--------|---------|--------|----------|-------|------|---------|---------\n";

            for (const auto& fold : folds)
            {
                // IS (train) row
                std::cout << "  " << std::setw(4) << fold.foldIndex << " IS  "
                          << fold.trainDateFrom << "→" << fold.trainDateTo << " "
                          << std::setw(4) << fold.trainBars << " "
                          << "$" << std::setw(8) << std::fixed << std::setprecision(0) << fold.trainPriceStart
                          << "→$" << std::setw(8) << fold.trainPriceEnd << " "
                          << std::setw(6) << std::setprecision(2) << fold.trainStats.sharpeRatio << " "
                          << std::setw(7) << fold.trainStats.returnPct << " "
                          << std::setw(6) << fold.trainStats.totalTrades << " "
                          << std::setw(8) << fold.trainStats.netPnl << " "
                          << std::setw(5) << (fold.trainStats.winRate * 100) << " "
                          << std::setw(4) << fold.trainStats.profitFactor << " "
                          << std::setw(7) << fold.trainStats.maxDrawdownPct << " "
                          << std::setw(8) << fold.trainStats.totalFees << "\n";

                // OOS (test) row
                std::cout << "     " << (fold.skipped ? "OOS SKIP" : "     OOS") << " "
                          << fold.testDateFrom << "→" << fold.testDateTo << " "
                          << std::setw(4) << fold.testBars << " "
                          << "$" << std::setw(8) << std::setprecision(0) << fold.testPriceStart
                          << "→$" << std::setw(8) << fold.testPriceEnd << " "
                          << std::setw(6) << std::setprecision(2) << fold.testStats.sharpeRatio << " "
                          << std::setw(7) << fold.testStats.returnPct << " "
                          << std::setw(6) << fold.testStats.totalTrades << " "
                          << std::setw(8) << fold.testStats.netPnl << " "
                          << std::setw(5) << (fold.testStats.winRate * 100) << " "
                          << std::setw(4) << fold.testStats.profitFactor << " "
                          << std::setw(7) << fold.testStats.maxDrawdownPct << " "
                          << std::setw(8) << fold.testStats.totalFees << "\n";

                // Degradation + pass/skip status
                std::string statusStr;
                if (fold.skipped)
                    statusStr = "SKIP (" + fold.skipReason + ")";
                else if (fold.passed)
                    statusStr = "PASS";
                else
                {
                    std::string reason;
                    if (fold.testStats.sharpeRatio <= 0.0)
                        reason = "OOS Sharpe=" + std::to_string(static_cast<int>(fold.testStats.sharpeRatio * 100) / 100.0) + "<=0";
                    else if (fold.degradation < wfoDegradation)
                        reason = "degradation " + std::to_string(static_cast<int>(fold.degradation * 100)) + "% < " + std::to_string(static_cast<int>(wfoDegradation * 100)) + "%";
                    statusStr = "FAIL (" + reason + ")";
                }
                std::cout << "     Degradation: " << std::setprecision(2) << fold.degradation
                          << " | " << statusStr << "\n";
            }

            // Cross-fold consistency summary
            double isSharpeSum = 0, oosSharpeSum = 0, isRetSum = 0, oosRetSum = 0;
            size_t nPassed = 0, nSkipped = 0, nonSkipped = 0;
            for (const auto& fold : folds)
            {
                if (fold.skipped) { nSkipped++; continue; }
                nonSkipped++;
                isSharpeSum += fold.trainStats.sharpeRatio;
                oosSharpeSum += fold.testStats.sharpeRatio;
                isRetSum += fold.trainStats.returnPct;
                oosRetSum += fold.testStats.returnPct;
                if (fold.passed) nPassed++;
            }
            if (nonSkipped > 0)
            {
                double avgIsSh = isSharpeSum / nonSkipped;
                double avgOosSh = oosSharpeSum / nonSkipped;
                double avgIsRet = isRetSum / nonSkipped;
                double avgOosRet = oosRetSum / nonSkipped;

                // Cross-fold Sharpe std dev
                double isVar = 0, oosVar = 0;
                for (const auto& fold : folds) {
                    if (fold.skipped) continue;
                    isVar += (fold.trainStats.sharpeRatio - avgIsSh) * (fold.trainStats.sharpeRatio - avgIsSh);
                    oosVar += (fold.testStats.sharpeRatio - avgOosSh) * (fold.testStats.sharpeRatio - avgOosSh);
                }
                double isStd = std::sqrt(isVar / nonSkipped);
                double oosStd = std::sqrt(oosVar / nonSkipped);

                std::cout << "  Summary: " << folds.size() << " folds | "
                          << nSkipped << " skipped | " << nPassed << "/" << nonSkipped << " passed ("
                          << std::setprecision(0) << (nonSkipped > 0 ? nPassed * 100.0 / nonSkipped : 0) << "%)\n";
                std::cout << "  Avg IS Sharpe=" << std::setprecision(2) << avgIsSh
                          << " (σ=" << isStd << ")"
                          << " | Avg OOS Sharpe=" << avgOosSh << " (σ=" << oosStd << ")\n";
                std::cout << "  Avg IS Return=" << std::setprecision(1) << avgIsRet
                          << "% | Avg OOS Return=" << avgOosRet << "%\n";
            }
            else
            {
                std::cout << "  Summary: " << folds.size() << " folds | ALL SKIPPED\n";
            }

            ++wfoShown;
        }
    }

    // Per-strategy breakdown
    printPerStrategySummary(results, plateau);

    // Statistical summary
    using Stats = OptimizationStatistics<CombinedParams, CombinedGrid>;
    Stats::printSummary(results);

    // ===== Step 6: Export =====
    fs::create_directories("results");
    std::string csvOut = "results/" + coin + "_grid_results.csv";
    exportRobustCSV(csvOut, robustRows, timeframe, dateFrom, dateTo, g_bars.size(), firstPrice, lastPrice);
    std::cerr << "\nExported " << robustRows.size() << " results to " << csvOut << "\n";

    if (wfoEnabled && !wfoAll.empty())
    {
        std::string wfoOut = "results/" + coin + "_wfo_results.csv";
        exportWfoCSV(wfoOut, wfoAll);
        std::cerr << "Exported WFO results to " << wfoOut << "\n";
    }

    return 0;
}
