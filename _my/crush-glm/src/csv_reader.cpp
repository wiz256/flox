#include "csv_reader.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>

namespace crush
{

std::vector<CsvBar> loadCsv(const std::string& path)
{
    std::vector<CsvBar> bars;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return bars;

    char buf[1024];
    if (!std::fgets(buf, sizeof(buf), f)) { std::fclose(f); return bars; }

    while (std::fgets(buf, sizeof(buf), f))
    {
        CsvBar b{};
        char* p = buf;
        auto skipWhitespace = [&]() { while (*p == ' ' || *p == '\t') ++p; };
        auto parseDouble = [&]() -> double {
            skipWhitespace();
            char* end = nullptr;
            double v = std::strtod(p, &end);
            p = end;
            if (*p == ',') ++p;
            return v;
        };
        auto parseInt64 = [&]() -> int64_t {
            skipWhitespace();
            char* end = nullptr;
            int64_t v = std::strtoll(p, &end, 10);
            p = end;
            if (*p == ',') ++p;
            return v;
        };

        b.timestamp_ms = parseInt64();
        b.open = parseDouble();
        b.high = parseDouble();
        b.low = parseDouble();
        b.close = parseDouble();
        b.volume = parseDouble();

        if (b.close > 0 && b.high >= b.low)
            bars.push_back(b);
    }
    std::fclose(f);

    std::sort(bars.begin(), bars.end(),
              [](const CsvBar& a, const CsvBar& b) { return a.timestamp_ms < b.timestamp_ms; });
    return bars;
}

std::vector<std::string> findCsvFiles(const std::string& dir, const std::string& tf)
{
    std::vector<std::string> files;
    namespace fs = std::filesystem;
    if (!fs::exists(dir)) return files;
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.size() < 5 || name.substr(name.size() - 4) != ".csv") continue;
        if (name.find("_" + tf + ".csv") != std::string::npos ||
            name.find("_" + tf + "_") != std::string::npos)
        {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace crush
