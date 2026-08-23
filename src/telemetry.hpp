#pragma once

#include "models.hpp"

#include <chrono>
#include <cstdint>
#include <string>

class TelemetryParser {
public:
    TelemetrySnapshot parse(const std::string& text, double latencyMs);
    static const std::string& probeScript();

private:
    std::uint64_t previousRx_ = 0;
    std::uint64_t previousTx_ = 0;
    std::chrono::steady_clock::time_point previousNetworkSample_{};
};
