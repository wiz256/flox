#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace crush
{

struct CsvBar
{
    int64_t timestamp_ms = 0;
    double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

std::vector<CsvBar> loadCsv(const std::string& path);
std::vector<std::string> findCsvFiles(const std::string& dir, const std::string& tf);

} // namespace crush
