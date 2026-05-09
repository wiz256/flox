#pragma once

#include "flox/indicator/ema.h"
#include "flox/indicator/atr.h"

#include <cmath>
#include <span>
#include <vector>

namespace flox::indicator
{

struct KeltnerResult
{
  std::vector<double> upper;
  std::vector<double> middle;
  std::vector<double> lower;
};

class Keltner
{
 public:
  explicit Keltner(size_t emaPeriod = 20, size_t atrPeriod = 10, double atrMult = 2.0) noexcept
      : _emaPeriod(emaPeriod), _atrPeriod(atrPeriod), _atrMult(atrMult)
  {
  }

  void update(double high, double low, double close)
  {
    _high.push_back(high);
    _low.push_back(low);
    _close.push_back(close);
    _dirty = true;
  }

  double upperValue() const { refresh(); return tail(_last.upper); }
  double middleValue() const { refresh(); return tail(_last.middle); }
  double lowerValue() const { refresh(); return tail(_last.lower); }
  double value() const { return middleValue(); }
  bool ready() const { refresh(); return !_last.middle.empty() && std::isfinite(_last.middle.back()); }
  void reset() { _high.clear(); _low.clear(); _close.clear(); _last = {}; _dirty = false; }
  size_t count() const noexcept { return _close.size(); }

  KeltnerResult compute(std::span<const double> high, std::span<const double> low,
                         std::span<const double> close) const
  {
    const size_t n = high.size();
    KeltnerResult r;
    r.upper.resize(n, std::nan(""));
    r.middle.resize(n, std::nan(""));
    r.lower.resize(n, std::nan(""));
    if (n == 0) return r;

    EMA ema(_emaPeriod);
    auto mid = ema.compute(close);
    ATR atr(_atrPeriod);
    auto atrVals = atr.compute(high, low, close);

    for (size_t i = 0; i < n; ++i)
    {
      r.middle[i] = mid[i];
      if (std::isfinite(mid[i]) && std::isfinite(atrVals[i]))
      {
        r.upper[i] = mid[i] + _atrMult * atrVals[i];
        r.lower[i] = mid[i] - _atrMult * atrVals[i];
      }
    }
    return r;
  }

  KeltnerResult compute(std::span<const double> close) const
  {
    return compute(_high.empty() ? std::vector<double>{} : _high,
                   _low.empty() ? std::vector<double>{} : _low, close);
  }

  size_t period() const noexcept { return _emaPeriod; }
  size_t emaPeriod() const noexcept { return _emaPeriod; }
  size_t atrPeriod() const noexcept { return _atrPeriod; }
  double atrMult() const noexcept { return _atrMult; }

 private:
  void refresh() const
  {
    if (!_dirty) return;
    if (_close.empty()) { _last = {}; _dirty = false; return; }
    _last = compute(std::span<const double>(_high),
                    std::span<const double>(_low),
                    std::span<const double>(_close));
    _dirty = false;
  }
  static double tail(const std::vector<double>& v) { return v.empty() ? std::nan("") : v.back(); }

  size_t _emaPeriod;
  size_t _atrPeriod;
  double _atrMult;
  std::vector<double> _high, _low, _close;
  mutable KeltnerResult _last;
  mutable bool _dirty = false;
};

}  // namespace flox::indicator
