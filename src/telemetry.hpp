#pragma once

#include "models.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

class TelemetryParser {
public:
    TelemetrySnapshot parse(const std::string& text, double latencyMs);
    static const std::string& probeScript();

private:
    struct RaplReading {
        std::uint64_t energyMicrojoules = 0;
        std::uint64_t maxEnergyRangeMicrojoules = 0;
    };

    std::string previousInterface_;
    std::uint64_t previousRx_ = 0;
    std::uint64_t previousTx_ = 0;
    std::chrono::steady_clock::time_point previousNetworkSample_{};
    std::unordered_map<std::string, RaplReading> previousRaplReadings_;
    std::chrono::steady_clock::time_point previousRaplSample_{};
};
