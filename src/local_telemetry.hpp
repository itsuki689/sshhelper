#pragma once

#include "models.hpp"
#include <memory>
#include <span>

// The sampler is constructed, sampled and destroyed on its monitoring thread.
class LocalTelemetry {
public:
    LocalTelemetry();
    ~LocalTelemetry();
    TelemetrySnapshot sample();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace local_telemetry {
struct CpuTimes { std::uint64_t idle, kernel, user; };
struct CpuSensorReading { std::string name, identifier, type; double value; };
double cpuUsage(CpuTimes previous, CpuTimes current);
double thermalZoneCelsius(double raw, bool highPrecision);
void applyCpuSensors(TelemetrySnapshot& snapshot, std::span<const CpuSensorReading> readings);
// SMBIOS formatted records (without the RawSMBIOSData header).
std::uint64_t configuredMemoryRate(std::span<const unsigned char> data);
}
