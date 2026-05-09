#pragma once

#include "flox/indicator/atr.h"
#include "flox/indicator/streaming.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct SupertrendResult
{
  std::vector<double> value;
  std::vector<int> direction;
};

class Supertrend : public StreamingBar<Supertrend>
{
 public:
  explicit Supertrend(size_t atrPeriod = 10, double atrMult = 3.0) noexcept
      : _atrPeriod(atrPeriod), _atrMult(atrMult)
  {
  }

  std::vector<double> compute(std::span<const double> high, std::span<const double> low,
                               std::span<const double> close) const
  {
    auto st = computeFull(high, low, close);
    return std::move(st.value);
  }

  SupertrendResult computeFull(std::span<const double> high, std::span<const double> low,
                                std::span<const double> close) const
  {
    const size_t n = close.size();
    SupertrendResult r;
    r.value.resize(n, std::nan(""));
    r.direction.resize(n, 0);
    if (n < _atrPeriod + 1) return r;

    ATR atr(_atrPeriod);
    auto atrVals = atr.compute(high, low, close);

    double upperBand = std::nan("");
    double lowerBand = std::nan("");
    int prevDir = 0;

    for (size_t i = _atrPeriod; i < n; ++i)
    {
      if (std::isnan(atrVals[i])) continue;
      double hl2 = (high[i] + low[i]) / 2.0;
      double rawUpper = hl2 + _atrMult * atrVals[i];
      double rawLower = hl2 - _atrMult * atrVals[i];

      if (std::isnan(lowerBand) || rawLower > lowerBand || close[i - 1] < lowerBand)
        lowerBand = rawLower;
      if (std::isnan(upperBand) || rawUpper < upperBand || close[i - 1] > upperBand)
        upperBand = rawUpper;

      int dir;
      if (prevDir == -1)
        dir = (close[i] > upperBand) ? 1 : -1;
      else
        dir = (close[i] < lowerBand) ? -1 : (prevDir == 0 ? 1 : prevDir);

      r.value[i] = (dir == 1) ? lowerBand : upperBand;
      r.direction[i] = dir;
      prevDir = dir;
    }
    return r;
  }

  size_t period() const noexcept { return _atrPeriod; }
  size_t atrPeriod() const noexcept { return _atrPeriod; }
  double atrMult() const noexcept { return _atrMult; }

 private:
  size_t _atrPeriod;
  double _atrMult;
};

}  // namespace flox::indicator
