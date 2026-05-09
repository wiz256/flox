/*
  Codex FLOX C++ MTF grid backtest sandbox.

  Build:
    cd /Users/lex/WorkspaceTrading/Trader7-Kilo/vendor/flox/_my/codex
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

  Smoke test:
    ./build/codex_mtf_grid_backtest --mode smoke

  Download Binance USD-M 1m candles and convert them to native mmap bars:
    python scripts/binance_futures_klines.py \
      --symbols BTCUSDT,ETHUSDT --interval 1m --start 2024-01-01 --out data/binance_csv

    ./build/codex_mtf_grid_backtest \
      --mode convert-csv \
      --csv data/binance_csv/BTCUSDT_1m.csv \
      --symbol BTCUSDT \
      --out-mmap data/mmap/BTCUSDT

  Run a C++ grid backtest on many symbols. Symbols are a grid dimension:
    ./build/codex_mtf_grid_backtest \
      --mode grid \
      --input-kind mmap \
      --data data/mmap \
      --symbols BTCUSDT,ETHUSDT,SOLUSDT \
      --strategies ema_crossover,donchian,keltner_breakout,keltner_squeeze,tsmom,rsi2,rsi_bb_mr \
      --threads 8 \
      --out results.csv

  .floxlog path:
    ./build/codex_mtf_grid_backtest --mode grid --input-kind floxlog \
      --data /path/to/floxlog_dir --symbols BTCUSDT --strategies ema_crossover

  Parquet path:
    C++ FLOX in this checkout does not link Arrow. Convert parquet to CSV first:
      python scripts/parquet_to_csv.py --in BTCUSDT_1m.parquet --out data/BTCUSDT_1m.csv
    Then run --mode convert-csv.

  Design choices:
    - Entries are evaluated on CLOSED H4 bars. This avoids lookahead because the
      strategy only reads bars already closed and stored by FLOX.
    - H1/M15 are confirmation filters.
    - Exits are checked on M15. That is usually more rational than waiting 4h
      to cut risk, while still being feasible with historical M1 candles.
      M5/M1 exits are possible, but only trust them if your backtest data
      supports that precision and your live feed will publish the same bars.
    - For tick/book alphas (CVD, footprint, imbalance, queue), use .floxlog.
      Candle bars cannot reconstruct true trade order or book queue.
*/

#include "flox/aggregator/bar.h"
#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/timeframe.h"
#include "flox/backtest/backtest_optimizer.h"
#include "flox/backtest/backtest_result.h"
#include "flox/backtest/backtest_runner.h"
#include "flox/backtest/mmap_bar_replay_source.h"
#include "flox/backtest/mmap_bar_storage.h"
#include "flox/backtest/mmap_bar_writer.h"
#include "flox/book/events/trade_event.h"
#include "flox/engine/symbol_registry.h"
#include "flox/indicator/indicator_pipeline.h"
#include "flox/replay/abstract_event_reader.h"
#include "flox/strategy/strategy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace flox;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t NS_PER_SEC = 1'000'000'000ULL;
constexpr uint64_t M1_NS = 60ULL * NS_PER_SEC;
constexpr uint64_t M5_NS = 5ULL * 60ULL * NS_PER_SEC;
constexpr uint64_t M15_NS = 15ULL * 60ULL * NS_PER_SEC;
constexpr uint64_t H1_NS = 60ULL * 60ULL * NS_PER_SEC;
constexpr uint64_t H4_NS = 4ULL * 60ULL * 60ULL * NS_PER_SEC;

enum class StrategyKind
{
  EmaCrossover,
  Donchian,
  KeltnerBreakout,
  KeltnerSqueeze,
  Tsmom,
  Rsi2,
  RsiBbMr,
};

enum class ExitKind
{
  Signal,
  SignalBeTrail,
  Chandelier,
  TrailAtr,
  Bracket,
  BarCloseExactSlTp,
};

struct Args
{
  std::string mode = "smoke";
  std::string inputKind = "mmap";
  fs::path data;
  fs::path csv;
  fs::path outMmap;
  fs::path out = "codex_grid_results.csv";
  std::vector<std::string> symbols{"BTCUSDT"};
  std::vector<StrategyKind> strategies{StrategyKind::EmaCrossover, StrategyKind::Donchian};
  size_t threads = 0;
};

std::vector<std::string> split(const std::string& s, char sep = ',')
{
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, sep))
  {
    item.erase(std::remove_if(item.begin(), item.end(), ::isspace), item.end());
    if (!item.empty())
    {
      out.push_back(item);
    }
  }
  return out;
}

StrategyKind parseStrategy(const std::string& s)
{
  if (s == "ema_crossover") return StrategyKind::EmaCrossover;
  if (s == "donchian") return StrategyKind::Donchian;
  if (s == "keltner_breakout") return StrategyKind::KeltnerBreakout;
  if (s == "keltner_squeeze") return StrategyKind::KeltnerSqueeze;
  if (s == "tsmom") return StrategyKind::Tsmom;
  if (s == "rsi2") return StrategyKind::Rsi2;
  if (s == "rsi_bb_mr") return StrategyKind::RsiBbMr;
  throw std::runtime_error("unknown strategy: " + s);
}

std::string toString(StrategyKind k)
{
  switch (k)
  {
    case StrategyKind::EmaCrossover: return "ema_crossover";
    case StrategyKind::Donchian: return "donchian";
    case StrategyKind::KeltnerBreakout: return "keltner_breakout";
    case StrategyKind::KeltnerSqueeze: return "keltner_squeeze";
    case StrategyKind::Tsmom: return "tsmom";
    case StrategyKind::Rsi2: return "rsi2";
    case StrategyKind::RsiBbMr: return "rsi_bb_mr";
  }
  return "unknown";
}

std::string toString(ExitKind k)
{
  switch (k)
  {
    case ExitKind::Signal: return "signal";
    case ExitKind::SignalBeTrail: return "signal_be_trail";
    case ExitKind::Chandelier: return "chandelier";
    case ExitKind::TrailAtr: return "trail_atr";
    case ExitKind::Bracket: return "bracket";
    case ExitKind::BarCloseExactSlTp: return "bar_close_exact_sl_tp";
  }
  return "unknown";
}

int64_t normalizeTsNs(int64_t raw)
{
  if (raw < static_cast<int64_t>(1e12)) return raw * 1'000'000'000LL;  // seconds
  if (raw < static_cast<int64_t>(1e15)) return raw * 1'000'000LL;      // millis
  if (raw < static_cast<int64_t>(1e18)) return raw * 1'000LL;          // micros
  return raw;                                                          // nanos
}

TimePoint tp(int64_t ns)
{
  return TimePoint{std::chrono::nanoseconds{ns}};
}

double closeOf(const Bar& b) { return b.close.toDouble(); }
double highOf(const Bar& b) { return b.high.toDouble(); }
double lowOf(const Bar& b) { return b.low.toDouble(); }

std::vector<Bar> parseCsvBars(const fs::path& path, uint64_t tfNs)
{
  std::ifstream f(path);
  if (!f.is_open())
  {
    throw std::runtime_error("cannot open csv: " + path.string());
  }

  std::vector<Bar> bars;
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line))
  {
    if (line.empty()) continue;
    std::stringstream ss(line);
    std::string tok;
    std::vector<std::string> cols;
    while (std::getline(ss, tok, ','))
    {
      cols.push_back(tok);
    }
    if (cols.size() < 6) continue;

    const int64_t startNs = normalizeTsNs(std::stoll(cols[0]));
    Bar b;
    b.startTime = tp(startNs);
    b.endTime = tp(startNs + static_cast<int64_t>(tfNs));
    b.open = Price::fromDouble(std::stod(cols[1]));
    b.high = Price::fromDouble(std::stod(cols[2]));
    b.low = Price::fromDouble(std::stod(cols[3]));
    b.close = Price::fromDouble(std::stod(cols[4]));
    b.volume = Volume::fromDouble(std::stod(cols[5]));
    b.buyVolume = Volume{};
    b.tradeCount = Quantity::fromDouble(1.0);
    b.reason = BarCloseReason::Threshold;
    bars.push_back(b);
  }
  std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b)
            { return a.endTime < b.endTime; });
  return bars;
}

std::vector<Bar> resampleTimeBars(const std::vector<Bar>& src, uint64_t tfNs)
{
  std::vector<Bar> out;
  if (src.empty()) return out;

  bool active = false;
  Bar cur;
  int64_t bucketStart = 0;
  for (const auto& b : src)
  {
    const int64_t startNs = b.startTime.time_since_epoch().count();
    const int64_t aligned = (startNs / static_cast<int64_t>(tfNs)) * static_cast<int64_t>(tfNs);
    if (!active || aligned != bucketStart)
    {
      if (active) out.push_back(cur);
      active = true;
      bucketStart = aligned;
      cur = {};
      cur.startTime = tp(bucketStart);
      cur.endTime = tp(bucketStart + static_cast<int64_t>(tfNs));
      cur.open = b.open;
      cur.high = b.high;
      cur.low = b.low;
      cur.close = b.close;
      cur.volume = b.volume;
      cur.buyVolume = b.buyVolume;
      cur.tradeCount = b.tradeCount;
      cur.reason = BarCloseReason::Threshold;
    }
    else
    {
      cur.high = Price::fromDouble(std::max(cur.high.toDouble(), b.high.toDouble()));
      cur.low = Price::fromDouble(std::min(cur.low.toDouble(), b.low.toDouble()));
      cur.close = b.close;
      cur.volume += b.volume;
      cur.buyVolume += b.buyVolume;
      cur.tradeCount += b.tradeCount;
    }
  }
  if (active) out.push_back(cur);
  return out;
}

std::vector<BarEvent> makeEvents(SymbolId sym, const std::vector<std::pair<TimeframeId, std::vector<Bar>>>& byTf)
{
  std::vector<BarEvent> events;
  for (const auto& [tf, bars] : byTf)
  {
    for (const auto& b : bars)
    {
      BarEvent ev;
      ev.symbol = sym;
      ev.instrument = InstrumentType::Future;
      ev.barType = tf.type;
      ev.barTypeParam = tf.param;
      ev.bar = b;
      events.push_back(ev);
    }
  }

  const auto rank = [](uint64_t param)
  {
    // Coarser bars first on tied timestamps so H4/H1 context is fresh before
    // M15/M5 exit callbacks at the same close instant.
    return static_cast<int64_t>(param);
  };
  std::sort(events.begin(), events.end(), [&](const BarEvent& a, const BarEvent& b)
            {
              if (a.bar.endTime != b.bar.endTime) return a.bar.endTime < b.bar.endTime;
              return rank(a.barTypeParam) > rank(b.barTypeParam);
            });
  return events;
}

std::vector<BarEvent> loadCsvEvents(const fs::path& path, SymbolId sym)
{
  auto m1 = parseCsvBars(path, M1_NS);
  return makeEvents(sym, {{timeframe::M1, m1},
                          {timeframe::M5, resampleTimeBars(m1, M5_NS)},
                          {timeframe::M15, resampleTimeBars(m1, M15_NS)},
                          {timeframe::H1, resampleTimeBars(m1, H1_NS)},
                          {timeframe::H4, resampleTimeBars(m1, H4_NS)}});
}

std::vector<BarEvent> loadMmapEvents(const fs::path& symbolDir, SymbolId sym)
{
  auto storage = std::make_unique<MmapBarStorage>(symbolDir);
  MmapBarReplaySource source(std::move(storage), sym);
  std::vector<BarEvent> events;
  source.replay([&](const BarEvent& ev)
                {
                  if (ev.barType == BarType::Time)
                  {
                    events.push_back(ev);
                  }
                });
  std::sort(events.begin(), events.end(), [](const BarEvent& a, const BarEvent& b)
            {
              if (a.bar.endTime != b.bar.endTime) return a.bar.endTime < b.bar.endTime;
              return a.barTypeParam > b.barTypeParam;
            });
  return events;
}

void writeMmapFromCsv(const fs::path& csv, const fs::path& outDir)
{
  auto m1 = parseCsvBars(csv, M1_NS);
  MmapBarWriter writer(outDir);
  writer.writeBars(timeframe::M1, m1);
  writer.writeBars(timeframe::M5, resampleTimeBars(m1, M5_NS));
  writer.writeBars(timeframe::M15, resampleTimeBars(m1, M15_NS));
  writer.writeBars(timeframe::H1, resampleTimeBars(m1, H1_NS));
  writer.writeBars(timeframe::H4, resampleTimeBars(m1, H4_NS));
  writer.setMetadata({{"source", csv.string()}, {"created_by", "codex_mtf_grid_backtest"}});
  writer.writeMetadata();
}

std::vector<double> closes(const std::vector<Bar>& bars)
{
  std::vector<double> v;
  v.reserve(bars.size());
  for (const auto& b : bars) v.push_back(closeOf(b));
  return v;
}

double sma(const std::vector<Bar>& bars, size_t n)
{
  if (bars.size() < n || n == 0) return std::numeric_limits<double>::quiet_NaN();
  double s = 0.0;
  for (size_t i = bars.size() - n; i < bars.size(); ++i) s += closeOf(bars[i]);
  return s / static_cast<double>(n);
}

double ema(const std::vector<Bar>& bars, size_t n)
{
  if (bars.size() < n || n == 0) return std::numeric_limits<double>::quiet_NaN();
  const double k = 2.0 / (static_cast<double>(n) + 1.0);
  double e = closeOf(bars[bars.size() - n]);
  for (size_t i = bars.size() - n + 1; i < bars.size(); ++i)
  {
    e = closeOf(bars[i]) * k + e * (1.0 - k);
  }
  return e;
}

double atr(const std::vector<Bar>& bars, size_t n)
{
  if (bars.size() < n + 1 || n == 0) return std::numeric_limits<double>::quiet_NaN();
  double s = 0.0;
  for (size_t i = bars.size() - n; i < bars.size(); ++i)
  {
    const double prevClose = closeOf(bars[i - 1]);
    const double tr = std::max({highOf(bars[i]) - lowOf(bars[i]),
                                std::abs(highOf(bars[i]) - prevClose),
                                std::abs(lowOf(bars[i]) - prevClose)});
    s += tr;
  }
  return s / static_cast<double>(n);
}

double rsi(const std::vector<Bar>& bars, size_t n)
{
  if (bars.size() < n + 1 || n == 0) return std::numeric_limits<double>::quiet_NaN();
  double gain = 0.0;
  double loss = 0.0;
  for (size_t i = bars.size() - n; i < bars.size(); ++i)
  {
    const double d = closeOf(bars[i]) - closeOf(bars[i - 1]);
    if (d >= 0) gain += d;
    else loss -= d;
  }
  if (loss <= 0.0) return 100.0;
  const double rs = gain / loss;
  return 100.0 - 100.0 / (1.0 + rs);
}

std::pair<double, double> bollinger(const std::vector<Bar>& bars, size_t n, double mult)
{
  if (bars.size() < n || n == 0) return {NAN, NAN};
  const double mean = sma(bars, n);
  double var = 0.0;
  for (size_t i = bars.size() - n; i < bars.size(); ++i)
  {
    const double d = closeOf(bars[i]) - mean;
    var += d * d;
  }
  const double sd = std::sqrt(var / static_cast<double>(n));
  return {mean - mult * sd, mean + mult * sd};
}

std::pair<double, double> minMax(const std::vector<Bar>& bars, size_t n, bool excludeLast = false)
{
  if (bars.size() < n + (excludeLast ? 1 : 0)) return {NAN, NAN};
  const size_t end = bars.size() - (excludeLast ? 1 : 0);
  const size_t begin = end - n;
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  for (size_t i = begin; i < end; ++i)
  {
    lo = std::min(lo, lowOf(bars[i]));
    hi = std::max(hi, highOf(bars[i]));
  }
  return {lo, hi};
}

struct PositionState
{
  int side = 0;  // +1 long, -1 short
  double entry = 0.0;
  double qty = 0.0;
  double stop = NAN;
  double takeProfit = NAN;
  double best = 0.0;
  double entryAtr = 0.0;
  int barsHeld = 0;
};

struct ExitConfig
{
  ExitKind kind = ExitKind::SignalBeTrail;
  int atrPeriod = 14;
  double trailAtrMult = 3.0;
  double slAtrMult = 2.0;
  double tpAtrMult = 1.5;
  int chandelierLookback = 22;
  double beTriggerAtr = 1.0;
  int timeStopBars = 96;
};

struct ExitEngine
{
  ExitConfig cfg;

  bool shouldExit(PositionState& p, const std::vector<Bar>& exitBars, bool oppositeSignal) const
  {
    if (p.side == 0 || exitBars.empty()) return false;
    const Bar& b = exitBars.back();
    const double c = closeOf(b);
    const double a = std::max(atr(exitBars, static_cast<size_t>(cfg.atrPeriod)), p.entryAtr);
    p.best = (p.side > 0) ? std::max(p.best, highOf(b)) : std::min(p.best, lowOf(b));
    ++p.barsHeld;

    if (oppositeSignal && (cfg.kind == ExitKind::Signal || cfg.kind == ExitKind::SignalBeTrail))
    {
      return true;
    }

    if (std::isfinite(p.stop))
    {
      if (p.side > 0 && c <= p.stop) return true;
      if (p.side < 0 && c >= p.stop) return true;
    }
    if (std::isfinite(p.takeProfit))
    {
      if (p.side > 0 && c >= p.takeProfit) return true;
      if (p.side < 0 && c <= p.takeProfit) return true;
    }

    if (!std::isfinite(a) || a <= 0.0) return false;

    if (cfg.kind == ExitKind::SignalBeTrail)
    {
      const double progress = p.side > 0 ? c - p.entry : p.entry - c;
      if (progress >= cfg.beTriggerAtr * a)
      {
        p.stop = p.side > 0 ? std::max(p.stop, p.entry) : std::min(p.stop, p.entry);
      }
      const double trail = p.side > 0 ? p.best - cfg.trailAtrMult * a : p.best + cfg.trailAtrMult * a;
      p.stop = p.side > 0 ? std::max(p.stop, trail) : std::min(p.stop, trail);
    }
    else if (cfg.kind == ExitKind::TrailAtr)
    {
      const double trail = p.side > 0 ? p.best - cfg.trailAtrMult * a : p.best + cfg.trailAtrMult * a;
      p.stop = p.side > 0 ? std::max(p.stop, trail) : std::min(p.stop, trail);
    }
    else if (cfg.kind == ExitKind::Chandelier)
    {
      auto [lo, hi] = minMax(exitBars, static_cast<size_t>(cfg.chandelierLookback));
      if (std::isfinite(lo) && std::isfinite(hi))
      {
        const double trail = p.side > 0 ? hi - cfg.trailAtrMult * a : lo + cfg.trailAtrMult * a;
        p.stop = p.side > 0 ? std::max(p.stop, trail) : std::min(p.stop, trail);
      }
    }

    if (cfg.timeStopBars > 0 && p.barsHeld >= cfg.timeStopBars)
    {
      return true;
    }
    return false;
  }
};

struct GridParams
{
  std::string symbol;
  StrategyKind strategy = StrategyKind::EmaCrossover;
  ExitKind exit = ExitKind::SignalBeTrail;
  int fast = 20;
  int slow = 50;
  int lookback = 55;
  double atrMult = 2.0;

  std::string toString() const
  {
    std::ostringstream os;
    os << "symbol=" << symbol << ",strategy=" << ::toString(strategy) << ",exit=" << ::toString(exit)
       << ",fast=" << fast << ",slow=" << slow << ",lookback=" << lookback
       << ",atr_mult=" << atrMult;
    return os.str();
  }
};

struct Grid
{
  std::vector<std::string> symbols;
  std::vector<StrategyKind> strategies;
  std::vector<ExitKind> exits{ExitKind::SignalBeTrail, ExitKind::Chandelier, ExitKind::Bracket};
  std::vector<int> fasts{10, 20};
  std::vector<int> slows{50, 100};
  std::vector<int> lookbacks{34, 55};
  std::vector<double> atrMults{1.5, 2.5};

  size_t totalCombinations() const
  {
    return symbols.size() * strategies.size() * exits.size() * fasts.size() * slows.size() *
           lookbacks.size() * atrMults.size();
  }

  GridParams operator[](size_t i) const
  {
    auto take = [&](size_t n)
    {
      const size_t v = i % n;
      i /= n;
      return v;
    };
    const size_t atrI = take(atrMults.size());
    const size_t lookI = take(lookbacks.size());
    const size_t slowI = take(slows.size());
    const size_t fastI = take(fasts.size());
    const size_t exitI = take(exits.size());
    const size_t stratI = take(strategies.size());
    const size_t symI = take(symbols.size());
    return GridParams{symbols[symI], strategies[stratI], exits[exitI], fasts[fastI],
                      slows[slowI], lookbacks[lookI], atrMults[atrI]};
  }
};

class CodexMtfStrategy final : public Strategy
{
 public:
  CodexMtfStrategy(SubscriberId id, const std::vector<SymbolId>& symbols, const SymbolRegistry& reg,
                   GridParams params)
      : Strategy(id, symbols, reg), _params(std::move(params))
  {
    // Increase built-in previous-bar storage from 64 to 256 for MTF work.
    setBarRingCapacity(256);
    _exit.cfg.kind = _params.exit;
    _exit.cfg.trailAtrMult = _params.atrMult;
    _exit.cfg.slAtrMult = _params.atrMult;
  }

 protected:
  void onSymbolBar(SymbolContext& ctx, const BarEvent& ev) override
  {
    if (ev.barType != BarType::Time) return;
    auto& st = _pos[ev.symbol];

    // Example of Strategy's built-in ring helpers. These are automatically
    // filled before this callback runs.
    const auto h4 = lastNClosedBars(ev.symbol, BarType::Time, H4_NS, 260);
    const auto h1 = lastNClosedBars(ev.symbol, BarType::Time, H1_NS, 260);
    const auto m15 = lastNClosedBars(ev.symbol, BarType::Time, M15_NS, 260);

    const bool enough = h4.size() >= 120 && h1.size() >= 80 && m15.size() >= 40;
    if (!enough) return;

    const int signal = computeSignal(h4, h1, m15);
    const bool opposite = st.side != 0 && signal == -st.side;

    // M15 exit checks are a pragmatic compromise: faster than H4, much less
    // noisy than M1, and available from broker M1 candles after resampling.
    if (ev.barTypeParam == M15_NS && st.side != 0)
    {
      if (_exit.shouldExit(st, m15, opposite))
      {
        closePosition(ev.symbol, st);
      }
      return;
    }

    // Entries only happen on H4 close. Confirmation timeframes are closed bars.
    if (ev.barTypeParam == H4_NS && st.side == 0 && signal != 0)
    {
      openPosition(ev.symbol, st, signal, closeOf(ev.bar), atr(m15, 14));
    }

    // A tiny IndicatorGraph example: in production you would keep a graph per
    // symbol and add reusable nodes once. Here we show the seed/step idea in a
    // debugger-friendly way without making it part of the trading decision.
    if (ev.barTypeParam == H4_NS)
    {
      _debugLastH4Sma[ev.symbol] = sma(h4, 20);
    }
  }

 private:
  int computeSignal(const std::vector<Bar>& h4, const std::vector<Bar>& h1,
                    const std::vector<Bar>& m15) const
  {
    const double c = closeOf(h4.back());
    const double h1Fast = ema(h1, 20);
    const double h1Slow = ema(h1, 50);
    const bool h1Bull = h1Fast > h1Slow;
    const bool h1Bear = h1Fast < h1Slow;

    switch (_params.strategy)
    {
      case StrategyKind::EmaCrossover:
      {
        const double fast = ema(h4, static_cast<size_t>(_params.fast));
        const double slow = ema(h4, static_cast<size_t>(_params.slow));
        if (fast > slow && h1Bull) return 1;
        if (fast < slow && h1Bear) return -1;
        return 0;
      }
      case StrategyKind::Donchian:
      {
        auto [lo, hi] = minMax(h4, static_cast<size_t>(_params.lookback), true);
        if (c > hi && h1Bull) return 1;
        if (c < lo && h1Bear) return -1;
        return 0;
      }
      case StrategyKind::KeltnerBreakout:
      {
        const double mid = ema(h4, static_cast<size_t>(_params.slow));
        const double a = atr(h4, 20);
        if (c > mid + _params.atrMult * a && h1Bull) return 1;
        if (c < mid - _params.atrMult * a && h1Bear) return -1;
        return 0;
      }
      case StrategyKind::KeltnerSqueeze:
      {
        const double aNow = atr(h4, 20);
        const double aSlow = atr(h4, 80);
        const double mid = ema(h4, static_cast<size_t>(_params.slow));
        if (aNow < 0.75 * aSlow) return 0;
        if (c > mid && h1Bull) return 1;
        if (c < mid && h1Bear) return -1;
        return 0;
      }
      case StrategyKind::Tsmom:
      {
        if (h4.size() < static_cast<size_t>(_params.lookback + 1)) return 0;
        const double prev = closeOf(h4[h4.size() - static_cast<size_t>(_params.lookback) - 1]);
        if (c > prev && h1Bull) return 1;
        if (c < prev && h1Bear) return -1;
        return 0;
      }
      case StrategyKind::Rsi2:
      {
        const double r = rsi(h4, 2);
        const double trend = ema(h4, 200);
        if (c > trend && r < 10.0) return 1;
        if (c < trend && r > 90.0) return -1;
        return 0;
      }
      case StrategyKind::RsiBbMr:
      {
        const double r = rsi(h4, 2);
        auto [lower, upper] = bollinger(h4, 20, 2.0);
        const double trend = ema(h4, 200);
        if (c > trend && r < 15.0 && c <= lower) return 1;
        if (c < trend && r > 85.0 && c >= upper) return -1;
        return 0;
      }
    }
    (void)m15;
    return 0;
  }

  void openPosition(SymbolId sym, PositionState& st, int side, double price, double a)
  {
    if (!std::isfinite(a) || a <= 0.0) a = price * 0.01;
    st.side = side;
    st.entry = price;
    st.qty = 1.0;
    st.entryAtr = a;
    st.best = price;
    st.barsHeld = 0;
    st.stop = side > 0 ? price - _exit.cfg.slAtrMult * a : price + _exit.cfg.slAtrMult * a;
    st.takeProfit = NAN;
    if (_params.exit == ExitKind::Bracket || _params.exit == ExitKind::BarCloseExactSlTp)
    {
      st.takeProfit = side > 0 ? price + _exit.cfg.tpAtrMult * a : price - _exit.cfg.tpAtrMult * a;
    }

    if (side > 0) emitMarketBuy(sym, Quantity::fromDouble(st.qty));
    else emitMarketSell(sym, Quantity::fromDouble(st.qty));
  }

  void closePosition(SymbolId sym, PositionState& st)
  {
    if (st.side > 0) emitMarketSell(sym, Quantity::fromDouble(st.qty));
    else if (st.side < 0) emitMarketBuy(sym, Quantity::fromDouble(st.qty));
    st = {};
  }

  GridParams _params;
  ExitEngine _exit;
  std::unordered_map<SymbolId, PositionState> _pos;
  std::unordered_map<SymbolId, double> _debugLastH4Sma;
};

BacktestResult runBarsBacktest(const GridParams& params, const std::vector<BarEvent>& events,
                               SymbolRegistry& reg, SymbolId sym)
{
  BacktestConfig cfg;
  cfg.initialCapital = 100000.0;
  cfg.feeRate = 0.0005;
  cfg.metricsAnnualizationFactor = 365.0 * 6.0;  // approximate H4 bars/year

  BacktestRunner runner(cfg);
  CodexMtfStrategy strategy(1, std::vector<SymbolId>{sym}, reg, params);
  runner.setStrategy(&strategy);
  return runner.runBars(events);
}

BacktestResult runFloxlogBacktest(const GridParams& params, const fs::path& dir, SymbolRegistry& reg,
                                  SymbolId sym)
{
  BacktestConfig cfg;
  cfg.initialCapital = 100000.0;
  cfg.feeRate = 0.0005;
  BacktestRunner runner(cfg);
  CodexMtfStrategy strategy(1, std::vector<SymbolId>{sym}, reg, params);
  runner.setStrategy(&strategy);

  BarBus bus;
  MultiTimeframeAggregator<5> agg(&bus);
  // Register coarsest-first for deterministic tied-close ordering.
  agg.addTimeInterval(std::chrono::seconds(4 * 60 * 60));
  agg.addTimeInterval(std::chrono::seconds(60 * 60));
  agg.addTimeInterval(std::chrono::seconds(15 * 60));
  agg.addTimeInterval(std::chrono::seconds(5 * 60));
  agg.addTimeInterval(std::chrono::seconds(60));
  bus.subscribe(&strategy);
  bus.start();
  agg.start();
  runner.addMarketDataSubscriber(&agg);

  replay::ReaderFilter filter;
  filter.symbols.insert(sym);
  auto reader = replay::createMultiSegmentReader(dir, filter);
  if (!reader) throw std::runtime_error("failed to open floxlog dir: " + dir.string());
  auto result = runner.run(*reader);
  agg.stop();
  bus.stop();
  return result;
}

std::vector<BarEvent> makeSynthetic(SymbolId sym)
{
  std::vector<Bar> m1;
  m1.reserve(40000);
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 7.0);
  double price = 30000.0;
  int64_t t = 1'700'000'000LL * 1'000'000'000LL;
  for (size_t i = 0; i < 40000; ++i)
  {
    price += 0.35 + noise(rng);
    const double o = price;
    const double c = price + noise(rng) * 0.2;
    Bar b;
    b.startTime = tp(t + static_cast<int64_t>(i * M1_NS));
    b.endTime = tp(t + static_cast<int64_t>((i + 1) * M1_NS));
    b.open = Price::fromDouble(o);
    b.close = Price::fromDouble(c);
    b.high = Price::fromDouble(std::max(o, c) + std::abs(noise(rng)));
    b.low = Price::fromDouble(std::min(o, c) - std::abs(noise(rng)));
    b.volume = Volume::fromDouble(100000.0);
    b.tradeCount = Quantity::fromDouble(1.0);
    m1.push_back(b);
    price = c;
  }
  return makeEvents(sym, {{timeframe::M1, m1},
                          {timeframe::M5, resampleTimeBars(m1, M5_NS)},
                          {timeframe::M15, resampleTimeBars(m1, M15_NS)},
                          {timeframe::H1, resampleTimeBars(m1, H1_NS)},
                          {timeframe::H4, resampleTimeBars(m1, H4_NS)}});
}

Args parseArgs(int argc, char** argv)
{
  Args a;
  for (int i = 1; i < argc; ++i)
  {
    const std::string key = argv[i];
    const auto need = [&]() -> std::string
    {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + key);
      return argv[++i];
    };
    if (key == "--mode") a.mode = need();
    else if (key == "--input-kind") a.inputKind = need();
    else if (key == "--data") a.data = need();
    else if (key == "--csv") a.csv = need();
    else if (key == "--out-mmap") a.outMmap = need();
    else if (key == "--out") a.out = need();
    else if (key == "--symbols") a.symbols = split(need());
    else if (key == "--threads") a.threads = static_cast<size_t>(std::stoul(need()));
    else if (key == "--strategies")
    {
      a.strategies.clear();
      for (const auto& s : split(need())) a.strategies.push_back(parseStrategy(s));
    }
    else
    {
      throw std::runtime_error("unknown arg: " + key);
    }
  }
  return a;
}

int runSmoke()
{
  SymbolRegistry reg;
  SymbolInfo info;
  info.exchange = "binance";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Future;
  info.tickSize = Price::fromDouble(0.01);
  const SymbolId sym = reg.registerSymbol(info);
  auto events = makeSynthetic(sym);

  GridParams p;
  p.symbol = "BTCUSDT";
  p.strategy = StrategyKind::EmaCrossover;
  p.exit = ExitKind::SignalBeTrail;
  auto result = runBarsBacktest(p, events, reg, sym);
  auto stats = result.computeStats();
  std::cout << "smoke bars=" << events.size() << " trades=" << stats.totalTrades
            << " net_pnl=" << stats.netPnl << " sharpe=" << stats.sharpeRatio << "\n";
  return 0;
}

int runGrid(const Args& args)
{
  SymbolRegistry reg;
  std::unordered_map<std::string, SymbolId> ids;
  std::unordered_map<std::string, std::vector<BarEvent>> eventsBySymbol;

  for (const auto& symbol : args.symbols)
  {
    SymbolInfo info;
    info.exchange = "binance";
    info.symbol = symbol;
    info.type = InstrumentType::Future;
    info.tickSize = Price::fromDouble(0.01);
    const SymbolId id = reg.registerSymbol(info);
    ids[symbol] = id;

    if (args.inputKind == "csv")
    {
      eventsBySymbol[symbol] = loadCsvEvents(args.data / (symbol + "_1m.csv"), id);
    }
    else if (args.inputKind == "mmap")
    {
      eventsBySymbol[symbol] = loadMmapEvents(args.data / symbol, id);
    }
    else if (args.inputKind == "floxlog")
    {
      // Loaded inside each run because it is streaming replay, not a bar vector.
    }
    else
    {
      throw std::runtime_error("input-kind must be csv, mmap, or floxlog");
    }
  }

  Grid grid;
  grid.symbols = args.symbols;
  grid.strategies = args.strategies;

  BacktestOptimizer<GridParams, Grid> optimizer;
  optimizer.setParameterGrid(grid);
  optimizer.setBacktestFactory(
      [&](const GridParams& p)
      {
        const SymbolId sym = ids.at(p.symbol);
        if (args.inputKind == "floxlog")
        {
          return runFloxlogBacktest(p, args.data, reg, sym);
        }
        return runBarsBacktest(p, eventsBySymbol.at(p.symbol), reg, sym);
      });
  optimizer.setProgressCallback(
      [](size_t done, size_t total, const OptimizationResult<GridParams>& latest)
      {
        if (done % 10 == 0 || done == total)
        {
          std::cout << "\rgrid " << done << "/" << total << " latest_sharpe="
                    << latest.sharpeRatio() << std::flush;
        }
      });

  auto results = optimizer.runLocal(args.threads);
  std::cout << "\n";
  auto ranked = decltype(optimizer)::rankResults(results, RankMetric::SharpeRatio);
  decltype(optimizer)::exportToCSV(ranked, args.out);
  for (size_t i = 0; i < std::min<size_t>(10, ranked.size()); ++i)
  {
    const auto& r = ranked[i];
    std::cout << i + 1 << ". " << r.parameters.toString() << " sharpe=" << r.sharpeRatio()
              << " return=" << r.totalReturn() << " trades=" << r.totalTrades() << "\n";
  }
  std::cout << "wrote " << args.out << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  try
  {
    const Args args = parseArgs(argc, argv);
    if (args.mode == "smoke")
    {
      return runSmoke();
    }
    if (args.mode == "convert-csv")
    {
      if (args.csv.empty() || args.outMmap.empty())
      {
        throw std::runtime_error("--mode convert-csv requires --csv and --out-mmap");
      }
      writeMmapFromCsv(args.csv, args.outMmap);
      std::cout << "wrote mmap bars -> " << args.outMmap << "\n";
      return 0;
    }
    if (args.mode == "grid")
    {
      return runGrid(args);
    }
    throw std::runtime_error("unknown --mode " + args.mode);
  }
  catch (const std::exception& e)
  {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
