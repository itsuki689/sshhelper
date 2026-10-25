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
        "HOST|  compute-node-01  \n"
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
    CHECK(snapshot.hostName == "compute-node-01");
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
    CHECK(next.hostName.empty());
    CHECK(parser.parse("HOST|\n", 1.0).hostName.empty());
    CHECK(next.networkDown > 0);
    CHECK(next.networkUp > 0);
    CHECK(next.networkRatesAvailable);

    // Missing/malformed samples and interface changes must reset the baseline.
    TelemetryParser networkParser;
    networkParser.parse("NET|eth0|1000000|2000000\n", 1.0);
    networkParser.parse("AIR|3\nEND|1\n", 1.0);
    auto recovered = networkParser.parse("NET|eth0|1000000|2000000\n", 1.0);
    CHECK(recovered.networkDown == 0 && recovered.networkUp == 0);
    CHECK(!recovered.networkRatesAvailable);
    auto switched = networkParser.parse("NET|wlan0|900000000|900000000\n", 1.0);
    CHECK(switched.networkDown == 0 && switched.networkUp == 0);
    CHECK(!switched.networkRatesAvailable);
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
    CHECK(active.networkRatesAvailable);

    // Link speed is independent of traffic rate and must not survive a missing sample.
    CHECK(parser.parse("NET|enp5s0|100|200|1000\n", 1.0).networkLinkMbps == 1000);
    CHECK(parser.parse("NET|enp5s0|100|200|2500\n", 1.0).networkLinkMbps == 2500);
    CHECK(parser.parse("NET|enp5s0|100|200|100000\n", 1.0).networkLinkMbps == 100000);
    for (const auto* speed : {"", "0", "-1", "4294967295", "18446744073709551616", "1000oops"}) {
        CHECK(parser.parse("NET|enp5s0|100|200|" + std::string(speed) + "\n", 1.0).networkLinkMbps == 0);
    }
    CHECK(parser.parse("NET|enp5s0|bad|200|1000\n", 1.0).networkLinkMbps == 0);
    CHECK(parser.parse("NET|enp5s0|100|200\n", 1.0).networkLinkMbps == 0);
    CHECK(parser.parse("END|1\n", 1.0).networkLinkMbps == 0);

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

    // Xeon E5-2690 v3: coretemp is available even when lm-sensors is absent.
    const std::string xeon =
        "CPU|Intel(R) Xeon(R) CPU E5-2690 v3 @ 2.60GHz|intel-rapl\n"
        "CPUTEMP|coretemp-hwmon1|Core 8|25.000\n"
        "CPUTEMP|coretemp-hwmon1|Core 9|27.000\n"
        "CPUTEMP|coretemp-hwmon1|Package id 0|32.000\n"
        "CPUTEMP|coretemp-hwmon1|Core 0|26.000\n";
    const auto direct = parser.parse(xeon, 1.0);
    CHECK(direct.packageTemperature == 32.0);
    CHECK(direct.coreTemperatures.size() == 3);
    CHECK(direct.packagePowerWatts < 0.0); // RAPL availability is independent.
    const std::string duplicate =
        "TEMP|coretemp-isa-0000|Package id 0|99.000\n"
        "TEMP|coretemp-isa-0000|Core 0|98.000\n";
    for (const auto& sample : {xeon + duplicate, duplicate + xeon}) {
        const auto preferred = parser.parse(sample, 1.0);
        CHECK(preferred.packageTemperature == 32.0);
        CHECK(preferred.coreTemperatures.size() == 3);
    }
    const auto dual = parser.parse(xeon +
        "CPUTEMP|coretemp-hwmon2|Physical id 1|40.000\n"
        "CPUTEMP|coretemp-hwmon2|Core 0|39.000\n"
        "CPUTEMP|coretemp-hwmon1|Package id 0|31.000\n"
        "TEMP|amdgpu-pci-0b00|edge|57.0\n", 1.0);
    CHECK(dual.packageTemperature == 40.0);
    CHECK(dual.coreTemperatures.size() == 4);
    CHECK(dual.gpus.size() == 1 && dual.gpus[0].temperature == 57.0);
    CHECK(parser.parse(
        "TEMP|coretemp-isa-0000|Core 0|48\n"
        "TEMP|coretemp-isa-0000|Core 1|52\n", 1.0).packageTemperature == 52.0);
    CHECK(parser.parse(
        "CPUTEMP|x86_pkg_temp-thermal_zone0|Package|33.0\n", 1.0).packageTemperature == 33.0);
    for (const auto* invalid : {"nan", "inf", "bad", "32garbage", "251", "-101", ""}) {
        const auto rejected = parser.parse(
            "TEMP|coretemp-isa-0000|Package id 0|41.0\n"
            "CPUTEMP|coretemp-hwmon1|Package id 0|" + std::string(invalid) + "\n", 1.0);
        CHECK(rejected.packageTemperature == 41.0);
    }
    CHECK(parser.parse("TEMP|nvme-pci-0100|Composite|35.85\n", 1.0).packageTemperature < 0);
    CHECK(parser.parse("AIR|3\nEND|1\n", 1.0).packageTemperature < 0);

    // SSD readings stay separate from CPU/GPU and disappear if unavailable.
    const auto ssd = parser.parse("SSDTEMP|hwmon0|29.850\nSSDTEMP|hwmon4|42.000\n", 1.0);
    CHECK(ssd.ssdTemperature == 42.0);
    CHECK(ssd.packageTemperature < 0 && ssd.gpus.empty());
    CHECK(parser.parse("SSDTEMP|hwmon0|0\n", 1.0).ssdTemperature == 0);
    for (const auto* value : {"nan", "inf", "bad", "30garbage", "150.001", "-1", ""}) {
        CHECK(parser.parse("SSDTEMP|hwmon0|" + std::string(value) + "\n", 1.0).ssdTemperature < 0);
    }
    CHECK(parser.parse("END|1\n", 1.0).ssdTemperature < 0);
    CHECK(parser.parse("GPU|0, NVIDIA, 40, 0, 0, 1024\n", 1.0).gpus[0].fanPercent == 0);
    for (const auto* value : {"N/A", "nan", "inf", "-1", "101"}) {
        CHECK(parser.parse("GPU|0, NVIDIA, 40, " + std::string(value) + ", 0, 1024\n", 1.0).gpus[0].fanPercent < 0);
    }

    CHECK(parser.parse("MEMCLOCK|2133\n", 1).memoryRateMTs == 2133);
    CHECK(parser.parse("END|1\n", 1).memoryRateMTs == 0);
    for (const auto* value : {"N/A", "-1", "0", "2133oops", "100001", "nan", ""})
        CHECK(parser.parse("MEMCLOCK|" + std::string(value) + "\n", 1).memoryRateMTs == 0);
    auto clocks = parser.parse("GPU|0, NVIDIA, 40, 27, 21525, 22528, 100, 6800\n", 1);
    CHECK(clocks.gpus[0].memoryClockMHz == 6800);
    CHECK(clocks.gpus[0].memoryUsed == 21525ULL * 1024 * 1024);
    CHECK(clocks.gpus[0].memoryTotal == 22528ULL * 1024 * 1024);
    CHECK(parser.parse("GPU|0, NVIDIA, 40, 27, 0, 22528\n", 1).gpus[0].memoryClockMHz < 0);
    for (const auto* value : {"N/A", "-1", "nan", "inf", "6800oops", "100001", ""})
        CHECK(parser.parse("GPU|0, NVIDIA, 40, 27, 0, 22528, 100, " + std::string(value) + "\n", 1).gpus[0].memoryClockMHz < 0);

    for (const auto* value : {"N/A", "-1", "21525oops", "30000", "18446744073709551615"})
        CHECK(parser.parse("GPU|0, NVIDIA, 40, 27, " + std::string(value) + ", 22528, 100, 6800\n", 1).gpus[0].memoryTotal == 0);
    CHECK(parser.parse("GPU|0, NVIDIA, 40, 27, 0, 22528, 100, 6800\n", 1).gpus[0].memoryTotal == 22528ULL * 1024 * 1024);

    const auto swapping = parser.parse("SWAP|1073741824|4294967296\n", 1);
    CHECK(swapping.swapAvailable && swapping.swapUsed == 1073741824ULL && swapping.swapTotal == 4294967296ULL);
    const auto noSwap = parser.parse("SWAP|0|0\n", 1);
    CHECK(noSwap.swapAvailable && noSwap.swapUsed == 0 && noSwap.swapTotal == 0);
    CHECK(!parser.parse("END|1\n", 1).swapAvailable);
    for (const auto* record : {"SWAP|-1|10\n", "SWAP|11|10\n", "SWAP|bad|10\n", "SWAP|1|bad\n", "SWAP|1|10oops\n", "SWAP|0|18446744073709551616\n"})
        CHECK(!parser.parse(record, 1).swapAvailable);

    TelemetryParser legacyParser;
    const auto legacy = legacyParser.parse(
        "AIR|2\nGPU|0, NVIDIA RTX 3080, 55, 30, 1024, 10240\nEND|1\n", 6.0);
    CHECK(legacy.gpus.size() == 1);
    CHECK(legacy.gpus[0].powerWatts < 0.0);

    std::cout << "telemetry parser ok\n";
    return 0;
}
