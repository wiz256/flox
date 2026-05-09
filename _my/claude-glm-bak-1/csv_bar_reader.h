#pragma once
// Minimal CSV reader for OHLCV bar data.
// Reads files with columns: timestamp,open,high,low,close,volume
// Timestamp can be Unix seconds, milliseconds, or ISO 8601.

#include "flox/aggregator/events/bar_event.h"
#include "flox/aggregator/bar.h"
#include "flox/common.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace flox::util
{

struct CsvBar
{
    int64_t timestampNs;  // Unix nanoseconds
    double open, high, low, close, volume;
};

inline int64_t parseTimestampToNs(const std::string& s)
{
    // Try numeric first
    try
    {
        size_t pos;
        double val = std::stod(s, &pos);
        if (pos == s.size())
        {
            // Heuristic: < 1e11 -> seconds, < 1e14 -> millis, else nanos
            if (val < 1e11)
                return static_cast<int64_t>(val * 1e9);
            else if (val < 1e14)
                return static_cast<int64_t>(val * 1e6);
            else
                return static_cast<int64_t>(val);
        }
    }
    catch (...)
    {
    }

    // ISO 8601: basic parsing
    // Format: YYYY-MM-DD HH:MM:SS or YYYY-MM-DDTHH:MM:SS
    int y, mo, d, h = 0, mi = 0, sec = 0;
    char sep;
    std::istringstream iss(s);
    iss >> y >> sep >> mo >> sep >> d;
    if (iss.peek() == ' ' || iss.peek() == 'T')
    {
        iss >> sep >> h >> sep >> mi >> sep >> sec;
    }
    // Simple conversion to Unix nanoseconds (UTC, no leap seconds)
    // This is approximate; for production use a proper date library
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = sec;
    t.tm_isdst = 0;
    time_t epoch = timegm(&t);
    return static_cast<int64_t>(epoch) * 1'000'000'000LL;
}

inline std::vector<CsvBar> readCsvBars(const std::string& filepath, bool hasHeader = true)
{
    std::vector<CsvBar> bars;
    std::ifstream file(filepath);
    if (!file.is_open()) return bars;

    std::string line;
    if (hasHeader) std::getline(file, line);  // skip header

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string ts, o, h, l, c, v;
        if (!std::getline(iss, ts, ',')) continue;
        if (!std::getline(iss, o, ',')) continue;
        if (!std::getline(iss, h, ',')) continue;
        if (!std::getline(iss, l, ',')) continue;
        if (!std::getline(iss, c, ',')) continue;
        std::getline(iss, v, ',');  // volume is optional

        CsvBar bar;
        bar.timestampNs = parseTimestampToNs(ts);
        bar.open = std::stod(o);
        bar.high = std::stod(h);
        bar.low = std::stod(l);
        bar.close = std::stod(c);
        bar.volume = v.empty() ? 0.0 : std::stod(v);
        bars.push_back(bar);
    }
    return bars;
}

inline std::vector<BarEvent> csvBarsToBarEvents(const std::vector<CsvBar>& csvBars,
                                                 SymbolId symbolId,
                                                 uint64_t barIntervalNs = 4 * 3600ULL * 1'000'000'000ULL)
{
    std::vector<BarEvent> events;
    events.reserve(csvBars.size());

    for (const auto& cb : csvBars)
    {
        BarEvent ev;
        ev.symbol = symbolId;
        ev.barType = BarType::Time;
        ev.barTypeParam = barIntervalNs;
        ev.bar.open = Price::fromDouble(cb.open);
        ev.bar.high = Price::fromDouble(cb.high);
        ev.bar.low = Price::fromDouble(cb.low);
        ev.bar.close = Price::fromDouble(cb.close);
        ev.bar.volume = Volume::fromDouble(cb.volume);
        ev.bar.startTime = TimePoint{} + std::chrono::nanoseconds(cb.timestampNs - static_cast<int64_t>(barIntervalNs));
        ev.bar.endTime = TimePoint{} + std::chrono::nanoseconds(cb.timestampNs);
        events.push_back(ev);
    }
    return events;
}

}  // namespace flox::util
