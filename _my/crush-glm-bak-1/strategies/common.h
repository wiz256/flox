#pragma once
#include <deque>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>

namespace flox_my {

struct OHLCV {
    double open, high, low, close, volume;
    int64_t timestamp;
};

inline double sma(const std::deque<double>& vals, size_t period) {
    if (vals.size() < period) return std::nan("");
    double sum = 0;
    auto it = vals.end();
    for (size_t i = 0; i < period; ++i) sum += *--it;
    return sum / static_cast<double>(period);
}

inline double ema(const std::deque<double>& vals, size_t period) {
    if (vals.size() < period) return std::nan("");
    double mult = 2.0 / (static_cast<double>(period) + 1.0);
    auto it = vals.end() - static_cast<ptrdiff_t>(period);
    double val = *it++;
    for (; it != vals.end(); ++it) val = *it * mult + val * (1.0 - mult);
    return val;
}

inline double rsi(const std::deque<double>& vals, size_t period) {
    if (vals.size() < period + 1) return std::nan("");
    double gain = 0, loss = 0;
    auto it = vals.end() - static_cast<ptrdiff_t>(period);
    for (size_t i = 0; i < static_cast<size_t>(period); ++i, ++it) {
        double delta = *it - *(it - 1);
        if (delta > 0) gain += delta; else loss -= delta;
    }
    gain /= static_cast<double>(period);
    loss /= static_cast<double>(period);
    if (loss == 0) return 100.0;
    double rs = gain / loss;
    return 100.0 - 100.0 / (1.0 + rs);
}

inline double atr(const std::deque<double>& closes, size_t period) {
    if (closes.size() < period + 1) return std::nan("");
    double sum = 0;
    auto it = closes.end() - static_cast<ptrdiff_t>(period);
    for (size_t i = 0; i < static_cast<size_t>(period); ++i, ++it) {
        double tr = std::abs(*it - *(it - 1));
        sum += tr;
    }
    return sum / static_cast<double>(period);
}

inline double roc(const std::deque<double>& vals, size_t period) {
    if (vals.size() < period + 1) return std::nan("");
    double current = vals.back();
    auto it = vals.end() - static_cast<ptrdiff_t>(period) - 1;
    double past = *it;
    if (past == 0) return 0;
    return (current - past) / past;
}

inline double rolling_std(const std::deque<double>& vals, size_t period) {
    if (vals.size() < period) return std::nan("");
    double mean = sma(vals, period);
    double sq_sum = 0;
    auto it = vals.end();
    for (size_t i = 0; i < period; ++i) {
        double d = *--it - mean;
        sq_sum += d * d;
    }
    return std::sqrt(sq_sum / static_cast<double>(period));
}

struct BBands {
    double upper, mid, lower;
};

inline BBands bollinger(const std::deque<double>& vals, size_t period, double std_mult) {
    BBands bb{std::nan(""), std::nan(""), std::nan("")};
    if (vals.size() < period) return bb;
    double mid = sma(vals, period);
    double sd = rolling_std(vals, period);
    bb.mid = mid;
    bb.upper = mid + std_mult * sd;
    bb.lower = mid - std_mult * sd;
    return bb;
}

struct KeltnerChannels {
    double upper, mid, lower;
};

inline KeltnerChannels keltner(const std::deque<double>& vals, size_t ema_period, size_t atr_period, double atr_mult) {
    KeltnerChannels kc{std::nan(""), std::nan(""), std::nan("")};
    if (vals.size() < std::max(ema_period, atr_period) + 1) return kc;
    kc.mid = ema(vals, ema_period);
    double a = atr(vals, atr_period);
    kc.upper = kc.mid + atr_mult * a;
    kc.lower = kc.mid - atr_mult * a;
    return kc;
}

inline double adx(const std::deque<double>& closes, size_t period) {
    if (closes.size() < period * 2 + 1) return std::nan("");
    std::deque<double> plus_dm, minus_dm, tr_vals;
    for (size_t i = 1; i < closes.size(); ++i) {
        double up = closes[i] - closes[i - 1];
        double down = closes[i - 1] - closes[i];
        plus_dm.push_back(up > down && up > 0 ? up : 0);
        minus_dm.push_back(down > up && down > 0 ? down : 0);
        tr_vals.push_back(std::abs(closes[i] - closes[i - 1]));
    }
    if (tr_vals.size() < period) return std::nan("");
    double sum_tr = 0, sum_plus = 0, sum_minus = 0;
    auto t_it = tr_vals.end() - static_cast<ptrdiff_t>(period);
    auto p_it = plus_dm.end() - static_cast<ptrdiff_t>(period);
    auto m_it = minus_dm.end() - static_cast<ptrdiff_t>(period);
    for (size_t i = 0; i < period; ++i) {
        sum_tr += *t_it++; sum_plus += *p_it++; sum_minus += *m_it++;
    }
    if (sum_tr == 0) return 0;
    double plus_di = 100.0 * sum_plus / sum_tr;
    double minus_di = 100.0 * sum_minus / sum_tr;
    double di_sum = plus_di + minus_di;
    if (di_sum == 0) return 0;
    return 100.0 * std::abs(plus_di - minus_di) / di_sum;
}

struct SupertrendResult {
    double value;
    int direction;
};

inline SupertrendResult supertrend(const std::deque<double>& closes, size_t atr_period, double atr_mult) {
    SupertrendResult res{std::nan(""), 0};
    if (closes.size() < atr_period + 2) return res;
    double a = atr(closes, atr_period);
    double close_cur = *std::prev(closes.end());
    double close_prev = *std::prev(closes.end(), 2);
    double half_range = a * atr_mult;
    double upper_band = close_cur + half_range;
    double lower_band = close_cur - half_range;
    res.value = lower_band;
    res.direction = close_cur > lower_band ? 1 : -1;
    return res;
}

template<typename T>
T shifted(const std::deque<T>& d, size_t offset) {
    if (d.size() < offset + 1) return T{};
    return *std::prev(d.end(), static_cast<ptrdiff_t>(offset) + 1);
}

} // namespace flox_my
