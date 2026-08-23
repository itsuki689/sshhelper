#include "telemetry.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    TelemetryParser parser;
    const std::string first =
        "AIR|2\n"
        "TEMP|coretemp-isa-0000|Package id 0|52.000\n"
        "TEMP|coretemp-isa-0000|Core 0|48.000\n"
        "TEMP|coretemp-isa-0000|Core 1|49.000\n"
        "FAN|nct6798-isa-0290|fan1|1250.000\n"
        "FAN|nct6798-isa-0290|fan3|0.000\n"
        "MEM|4294967296|8589934592\n"
        "DISK|/|107374182400|214748364800\n"
        "NET|eth0|1000000|2000000\n"
        "GPU|0, NVIDIA RTX 4090, 61, 44, 2048, 24564\n"
        "END|1\n";
    auto snapshot = parser.parse(first, 12.5);
    assert(snapshot.connected);
    assert(snapshot.packageTemperature == 52.0);
    assert(snapshot.coreTemperatures.size() == 2);
    assert(snapshot.fans.size() == 1);
    assert(snapshot.fans[0].label == "fan1");
    assert(snapshot.memoryUsed == 4294967296ULL);
    assert(snapshot.diskTotal == 214748364800ULL);
    assert(snapshot.gpus.size() == 1);
    assert(snapshot.gpus[0].temperature == 61.0);
    assert(snapshot.gpus[0].fanPercent == 44.0);
    assert(snapshot.sshLatencyMs == 12.5);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::string second =
        "AIR|2\nMEM|1|2\nDISK|/|1|2\nNET|eth0|1020000|2030000\nEND|1\n";
    auto next = parser.parse(second, 10.0);
    assert(next.networkDown > 0);
    assert(next.networkUp > 0);
    TelemetryParser amdParser;
    const auto amd = amdParser.parse(
        "AIR|2\nTEMP|amdgpu-pci-0b00|edge|57.0\nFAN|amdgpu-pci-0b00|fan1|1420\nEND|1\n", 8.0);
    assert(amd.gpus.size() == 1);
    assert(amd.gpus[0].temperature == 57.0);
    assert(amd.gpus[0].fanRpm == 1420.0);

    TelemetryParser fallbackParser;
    const auto fallback = fallbackParser.parse(
        "AIR|2\nTEMP|cpu_thermal-virtual-0|temp1|46.0\nFAN|pwmfan-isa-0000|fan3|0\nEND|1\n", 7.0);
    assert(fallback.packageTemperature == 46.0);
    assert(fallback.fans.empty());

    std::cout << "telemetry parser ok\n";
    return 0;
}
