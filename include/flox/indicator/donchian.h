#pragma once

#include "flox/indicator/streaming.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace flox::indicator
{

struct DonchianResult
{
  std::vector<double> upper;
  std::vector<double> lower;
};

class Donchian : public StreamingHighLow<Donchian>
{
 public:
  explicit Donchian(size_t period = 20) noexcept : _period(period) {}

  std::vector<double> compute(std::span<const double> high, std::span<const double> low) const
  {
    const size_t n = high.size();
    std::vector<double> out(n, std::nan(""));
    if (n < _period) return out;

    for (size_t i = _period - 1; i < n; ++i)
    {
      double maxH = *std::max_element(high.begin() + ptrdiff_t(i) - ptrdiff_t(_period) + 1,
                                       high.begin() + ptrdiff_t(i) + 1);
      double minL = *std::min_element(low.begin() + ptrdiff_t(i) - ptrdiff_t(_period) + 1,
                                       low.begin() + ptrdiff_t(i) + 1);
      out[i] = maxH - minL;
    }
    return out;
  }

  DonchianResult computeChannels(std::span<const double> high, std::span<const double> low) const
  {
    const size_t n = high.size();
    DonchianResult r;
    r.upper.resize(n, std::nan(""));
    r.lower.resize(n, std::nan(""));
    if (n < _period) return r;

    for (size_t i = _period - 1; i < n; ++i)
    {
      r.upper[i] = *std::max_element(high.begin() + ptrdiff_t(i) - ptrdiff_t(_period) + 1,
                                      high.begin() + ptrdiff_t(i) + 1);
      r.lower[i] = *std::min_element(low.begin() + ptrdiff_t(i) - ptrdiff_t(_period) + 1,
                                      low.begin() + ptrdiff_t(i) + 1);
    }
    return r;
  }

  size_t period() const noexcept { return _period; }

 private:
  size_t _period;
};

}  // namespace flox::indicator

#include "flox/indicator/indicator.h"
static_assert(flox::indicator::HasPeriod<flox::indicator::Donchian>);
