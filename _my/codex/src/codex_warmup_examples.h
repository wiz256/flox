#pragma once

// Small, debugger-friendly warmup examples for FLOX C++ strategies.
// This file is not needed by the demo executable; it is here as a copy/paste
// reference for live strategies.

#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/timeframe.h"
#include "flox/indicator/atr.h"
#include "flox/indicator/ema.h"
#include "flox/indicator/indicator_pipeline.h"
#include "flox/engine/symbol_registry.h"
#include "flox/strategy/strategy.h"

#include <array>
#include <span>
#include <vector>

namespace codex_flox_examples
{

inline void warmupBarMatrix(flox::SymbolId sym, std::span<const flox::Bar> h4History)
{
  flox::BarMatrix<256, 4, 256> matrix;
  std::array<flox::TimeframeId, 4> tfs = {
      flox::timeframe::H4,
      flox::timeframe::H1,
      flox::timeframe::M15,
      flox::timeframe::M5,
  };
  matrix.configure(tfs);
  matrix.warmup(sym, flox::timeframe::H4, h4History);

  // lag=0 is newest closed H4, lag=1 is the previous closed H4.
  const flox::Bar* latestH4 = matrix.bar(sym, flox::timeframe::H4, 0);
  (void)latestH4;
}

class RingCapacityExample : public flox::Strategy
{
 public:
  RingCapacityExample(flox::SubscriberId id, flox::SymbolId sym, const flox::SymbolRegistry& reg)
      : flox::Strategy(id, sym, reg)
  {
    // Default is 64. Use 256 when H4 strategies need long lookbacks.
    setBarRingCapacity(256);
  }

 protected:
  void onSymbolBar(flox::SymbolContext&, const flox::BarEvent& ev) override
  {
    const auto last256 = lastNClosedBars(ev.symbol, flox::BarType::Time,
                                         flox::timeframe::H4.param, 256);
    if (last256.size() < 200)
    {
      return;  // still warming up
    }
  }
};

inline double indicatorGraphExample(flox::SymbolId sym, const std::vector<flox::Bar>& bars)
{
  using namespace flox::indicator;

  IndicatorGraph graph;
  graph.setBars(sym, bars);
  graph.addNode("ema50", {}, [](IndicatorGraph& g, flox::SymbolId s)
                { return EMA(50).compute(g.close(s)); });
  graph.addNode("atr14", {}, [](IndicatorGraph& g, flox::SymbolId s)
                { return ATR(14).compute(g.high(s), g.low(s), g.close(s)); });
  graph.addNode("ema_slope_per_atr", {"ema50", "atr14"},
                [](IndicatorGraph& g, flox::SymbolId s)
                {
                  const auto& ema = *g.get(s, "ema50");
                  const auto& atr = *g.get(s, "atr14");
                  std::vector<double> out(ema.size(), 0.0);
                  for (size_t i = 1; i < ema.size(); ++i)
                  {
                    out[i] = atr[i] > 0.0 ? (ema[i] - ema[i - 1]) / atr[i] : 0.0;
                  }
                  return out;
                });

  const auto& value = graph.require(sym, "ema_slope_per_atr");
  return value.empty() ? 0.0 : value.back();
}

}  // namespace codex_flox_examples
