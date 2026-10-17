#include "local_telemetry.hpp"
#include <chrono>
#include <cmath>
#include <limits>
#include <iostream>
#include <thread>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n"; return __LINE__; } } while (false)

int main() {
    std::vector<unsigned char> table;
    auto dimm = [&](unsigned rate, bool installed) {
        const auto start = table.size(); table.resize(start + 36, 0);
        table[start] = 17; table[start + 1] = 34;
        table[start + 12] = installed ? 8 : 0;
        table[start + 32] = static_cast<unsigned char>(rate);
        table[start + 33] = static_cast<unsigned char>(rate >> 8);
    };
    dimm(3200, true); dimm(2133, true); dimm(1600, false);
    CHECK(local_telemetry::configuredMemoryRate(table) == 2133);
    CHECK(local_telemetry::configuredMemoryRate({}) == 0);
    for (std::size_t size = 0; size < 34; ++size)
        CHECK(local_telemetry::configuredMemoryRate(std::span(table).first(size)) == 0);
    table[1] = 0; CHECK(local_telemetry::configuredMemoryRate(table) == 0);

    using local_telemetry::CpuTimes;
    CHECK(local_telemetry::cpuUsage(CpuTimes{100, 200, 100}, CpuTimes{150, 300, 200}) == 75.0);
    CHECK(local_telemetry::cpuUsage(CpuTimes{100, 200, 100}, CpuTimes{100, 200, 100}) < 0);
    CHECK(local_telemetry::cpuUsage(CpuTimes{100, 200, 100}, CpuTimes{99, 300, 200}) < 0);
    CHECK(local_telemetry::cpuUsage(CpuTimes{0, 0, 0}, CpuTimes{100, 10, 20}) < 0);
    CHECK(std::abs(local_telemetry::thermalZoneCelsius(3354, true) - 62.25) < 0.001);
    CHECK(std::abs(local_telemetry::thermalZoneCelsius(335, false) - 61.85) < 0.001);
    CHECK(local_telemetry::thermalZoneCelsius(0, true) < 0);
    CHECK(local_telemetry::thermalZoneCelsius(std::numeric_limits<double>::quiet_NaN(), false) < 0);
    const std::vector<local_telemetry::CpuSensorReading> ryzen = {
        {"CCD1 (Tdie)", "/amdcpu/0/temperature/1", "Temperature", 80},
        {"CCD2 (Tdie)", "/amdcpu/0/temperature/2", "Temperature", 75},
        {"CCDs Max (Tdie)", "/amdcpu/0/temperature/3", "Temperature", 80},
        {"Core (Tctl/Tdie)", "/amdcpu/0/temperature/0", "Temperature", 62},
        {"CPU Package", "/amdcpu/0/power/0", "Power", 24},
        {"CPU PPT Limit", "/amdcpu/0/power/1", "Power", 120},
        {"Core", "/amdcpu/0/power/2", "Power", 8},
        {"CPU Package", "/acpi/0/temperature/0", "Temperature", 99},
    };
    TelemetrySnapshot cpu;
    local_telemetry::applyCpuSensors(cpu, ryzen);
    CHECK(cpu.packageTemperature == 62 && cpu.packagePowerWatts == 24);
    CHECK(cpu.chipletTemperatures.size() == 2 && cpu.coreTemperatures.empty());
    TelemetrySnapshot ccdOnly;
    local_telemetry::applyCpuSensors(ccdOnly, std::span(ryzen).first(3));
    CHECK(ccdOnly.packageTemperature < 0 && ccdOnly.chipletTemperatures.size() == 2);
    const std::vector<local_telemetry::CpuSensorReading> priority = {
        {"Core (Tdie)", "/amdcpu/0/temperature/1", "Temperature", 60},
        {"Core (Tctl)", "/amdcpu/0/temperature/0", "Temperature", 80},
        {"CPU Package", "/amdcpu/0/temperature/2", "Temperature", 70},
        {"CPU Package", "/amdcpu/0/power/0", "Power", std::numeric_limits<double>::quiet_NaN()},
    };
    TelemetrySnapshot chosen;
    local_telemetry::applyCpuSensors(chosen, priority);
    CHECK(chosen.packageTemperature == 60 && chosen.packagePowerWatts < 0);

    // Uses only read-only Windows APIs. No SSH, drivers or settings changes.
    LocalTelemetry sampler;
    const auto first = sampler.sample();
    CHECK(first.connected && first.status == "Local" && !first.hostName.empty());
    CHECK(first.memoryTotal > 0 && first.memoryUsed <= first.memoryTotal);
    CHECK(first.diskTotal > 0 && first.diskUsed <= first.diskTotal);
    CHECK(first.networkDown == 0 && first.networkUp == 0);
    CHECK(!first.swapAvailable || first.swapUsed <= first.swapTotal);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto next = sampler.sample();
    CHECK(next.connected && next.hostName == first.hostName);
    CHECK(next.cpuUsagePercent >= 0 && next.cpuUsagePercent <= 100);
    CHECK(next.cpuLogicalProcessors > 0 && next.cpuPhysicalCores > 0 && next.cpuPhysicalCores <= next.cpuLogicalProcessors);
    for (const auto& gpu : next.gpus) CHECK(gpu.memoryUsed <= gpu.memoryTotal);
    std::cout << "Local host: " << next.hostName << "\nRAM bytes: " << next.memoryUsed << '/' << next.memoryTotal
              << "\nDisk bytes: " << next.diskUsed << '/' << next.diskTotal
              << "\nConfigured RAM: " << next.memoryRateMTs << " MT/s\nPagefile bytes: " << next.swapUsed
              << '/' << next.swapTotal << "\nNetwork: " << next.networkLinkMbps << " Mbps\nSSD: " << next.ssdTemperature << " C\n";
    std::cout << "CPU: " << next.cpuName << "; usage: " << next.cpuUsagePercent
              << "; clock: " << next.cpuClockMHz << " MHz; topology: " << next.cpuPhysicalCores << "C/" << next.cpuLogicalProcessors
              << "T; CPU temperature: " << next.packageTemperature << "; thermal zone: " << next.thermalZoneTemperature
              << "; package power: " << next.packagePowerWatts << '\n';
    for (const auto& gpu : next.gpus)
        std::cout << "GPU: " << gpu.name << "; VRAM: " << gpu.memoryUsed << '/' << gpu.memoryTotal
                  << "; temperature: " << gpu.temperature << "; clock: " << gpu.memoryClockMHz
                  << "; fan: " << gpu.fanPercent << "; power: " << gpu.powerWatts << '\n';
    return 0;
}
