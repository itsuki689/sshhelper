#include "telemetry.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
        return __LINE__; \
    } \
} while (false)

int main() {
    TelemetryParser parser;
    const std::string first =
        "AIR|3\n"
        "CPU|Intel(R) Xeon(R) CPU E5-2667 v3 @ 3.20GHz|intel-rapl\n"
        "TEMP|coretemp-isa-0000|Package id 0|52.000\n"
        "TEMP|coretemp-isa-0000|Core 0|48.000\n"
        "TEMP|coretemp-isa-0000|Core 1|49.000\n"
        "POWER|coretemp-isa-0000|Package id 0|64.250\n"
        "FAN|nct6798-isa-0290|fan1|1250.000\n"
        "FAN|nct6798-isa-0290|fan3|0.000\n"
        "MEM|4294967296|8589934592\n"
        "DISK|/|107374182400|214748364800\n"
        "NET|eth0|1000000|2000000\n"
        "GPU|0, NVIDIA RTX 4090, 61, 44, 2048, 24564, 321.750\n"
        "END|1\n";
    auto snapshot = parser.parse(first, 12.5);
    CHECK(snapshot.connected);
    CHECK(snapshot.cpuName == "Intel(R) Xeon(R) CPU E5-2667 v3 @ 3.20GHz");
    CHECK(snapshot.cpuPowerStrategy == "intel-rapl");
    CHECK(snapshot.packageTemperature == 52.0);
    CHECK(snapshot.packagePowerWatts == 64.25);
    CHECK(snapshot.coreTemperatures.size() == 2);
    CHECK(snapshot.fans.size() == 1);
    CHECK(snapshot.fans[0].label == "fan1");
    CHECK(snapshot.memoryUsed == 4294967296ULL);
    CHECK(snapshot.diskTotal == 214748364800ULL);
    CHECK(snapshot.gpus.size() == 1);
    CHECK(snapshot.gpus[0].temperature == 61.0);
    CHECK(snapshot.gpus[0].powerWatts == 321.75);
    CHECK(snapshot.gpus[0].fanPercent == 44.0);
    CHECK(snapshot.sshLatencyMs == 12.5);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::string second =
        "AIR|2\nMEM|1|2\nDISK|/|1|2\nNET|eth0|1020000|2030000\nEND|1\n";
    auto next = parser.parse(second, 10.0);
    CHECK(next.networkDown > 0);
    CHECK(next.networkUp > 0);

    // Missing/malformed samples and interface changes must reset the baseline.
    TelemetryParser networkParser;
    networkParser.parse("NET|eth0|1000000|2000000\n", 1.0);
    networkParser.parse("AIR|3\nEND|1\n", 1.0);
    auto recovered = networkParser.parse("NET|eth0|1000000|2000000\n", 1.0);
    CHECK(recovered.networkDown == 0 && recovered.networkUp == 0);
    auto switched = networkParser.parse("NET|wlan0|900000000|900000000\n", 1.0);
    CHECK(switched.networkDown == 0 && switched.networkUp == 0);
    auto reset = networkParser.parse("NET|wlan0|10|20\n", 1.0);
    CHECK(reset.networkDown == 0 && reset.networkUp == 0);
    networkParser.parse("NET|wlan0|invalid|30\n", 1.0);
    auto afterInvalid = networkParser.parse("NET|wlan0|1000|2000\n", 1.0);
    CHECK(afterInvalid.networkDown == 0 && afterInvalid.networkUp == 0);
    networkParser.parse("NET|wlan0|-1|2000\n", 1.0);
    auto afterNegative = networkParser.parse("NET|wlan0|2000|3000\n", 1.0);
    CHECK(afterNegative.networkDown == 0 && afterNegative.networkUp == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto active = networkParser.parse("NET|wlan0|4000|6000\n", 1.0);
    CHECK(active.networkDown > 0 && active.networkUp > 0);

    TelemetryParser raplParser;
    const auto raplWarmup = raplParser.parse(
        "AIR|3\nCPU|Intel(R) Xeon(R) CPU E5-2667 v3 @ 3.20GHz|intel-rapl\n"
        "RAPL|package-0|1000000|10000000\nRAPL|package-1|2000000|10000000\nEND|1\n", 5.0);
    CHECK(raplWarmup.packagePowerWatts < 0.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto rapl = raplParser.parse(
        "AIR|3\nCPU|Intel(R) Xeon(R) CPU E5-2667 v3 @ 3.20GHz|intel-rapl\n"
        "RAPL|package-0|1500000|10000000\nRAPL|package-1|2500000|10000000\nEND|1\n", 5.0);
    CHECK(rapl.packagePowerWatts > 0.0);
    CHECK(rapl.packagePowerWatts < 100.0);

    TelemetryParser wrapParser;
    wrapParser.parse(
        "AIR|3\nCPU|Intel(R) Xeon(R) CPU E5-2667 v3|intel-rapl\n"
        "RAPL|package-0|9900000|10000000\nEND|1\n", 5.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto wrapped = wrapParser.parse(
        "AIR|3\nCPU|Intel(R) Xeon(R) CPU E5-2667 v3|intel-rapl\n"
        "RAPL|package-0|100000|10000000\nEND|1\n", 5.0);
    CHECK(wrapped.packagePowerWatts > 0.0);
    CHECK(wrapped.packagePowerWatts < 20.0);

    TelemetryParser amdParser;
    const auto amd = amdParser.parse(
        "AIR|2\nTEMP|amdgpu-pci-0b00|edge|57.0\nPOWER|amdgpu-pci-0b00|PPT|180.5\nFAN|amdgpu-pci-0b00|fan1|1420\nEND|1\n", 8.0);
    CHECK(amd.gpus.size() == 1);
    CHECK(amd.gpus[0].temperature == 57.0);
    CHECK(amd.gpus[0].powerWatts == 180.5);
    CHECK(amd.gpus[0].fanRpm == 1420.0);

    TelemetryParser fallbackParser;
    const auto fallback = fallbackParser.parse(
        "AIR|2\nTEMP|cpu_thermal-virtual-0|temp1|46.0\nPOWER|fam15h_power-pci-00c4|PPT|73.125\nFAN|pwmfan-isa-0000|fan3|0\nEND|1\n", 7.0);
    CHECK(fallback.packageTemperature == 46.0);
    CHECK(fallback.packagePowerWatts == 73.125);
    CHECK(fallback.fans.empty());

    TelemetryParser legacyParser;
    const auto legacy = legacyParser.parse(
        "AIR|2\nGPU|0, NVIDIA RTX 3080, 55, 30, 1024, 10240\nEND|1\n", 6.0);
    CHECK(legacy.gpus.size() == 1);
    CHECK(legacy.gpus[0].powerWatts < 0.0);

    std::cout << "telemetry parser ok\n";
    return 0;
}
