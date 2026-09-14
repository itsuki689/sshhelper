#include "telemetry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <sstream>

namespace {
std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& line, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string part;
    while (std::getline(stream, part, delimiter)) parts.push_back(trim(part));
    return parts;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool isGpuChip(const std::string& chip) {
    const auto value = lower(chip);
    return value.find("amdgpu") != std::string::npos ||
           value.find("nouveau") != std::string::npos ||
           value.find("i915") != std::string::npos;
}

bool isCpuChip(const std::string& chip) {
    const auto value = lower(chip);
    return value.find("coretemp") != std::string::npos ||
           value.find("k10temp") != std::string::npos ||
           value.find("zenpower") != std::string::npos ||
           value.find("fam15h_power") != std::string::npos ||
           value.find("intel_rapl") != std::string::npos ||
           value.find("cpu_thermal") != std::string::npos ||
           value.find("soc_thermal") != std::string::npos ||
           value.find("acpitz") != std::string::npos;
}

std::uint64_t u64(const std::string& value) {
    try { return std::stoull(value); } catch (...) { return 0; }
}

double real(const std::string& value) {
    try { return std::stod(value); } catch (...) { return -1.0; }
}
}

const std::string& TelemetryParser::probeScript() {
    static const std::string script = R"SH(#!/bin/sh
LC_ALL=C
printf 'AIR|3\n'

cpu_name=$(awk -F: '/^model name[[:space:]]*:/ {
  value=$0; sub(/^[^:]*:[[:space:]]*/, "", value); gsub(/\|/, "/", value); print value; exit
}' /proc/cpuinfo 2>/dev/null)
if [ -z "$cpu_name" ] && [ -r /proc/device-tree/model ]; then
  cpu_name=$(tr '\000|' ' /' </proc/device-tree/model 2>/dev/null)
fi

cpu_power_strategy=lm-sensors
case "$cpu_name" in
  *Intel*|*Xeon*) cpu_power_strategy=intel-rapl ;;
esac
printf 'CPU|%s|%s\n' "$cpu_name" "$cpu_power_strategy"

if [ "$cpu_power_strategy" = intel-rapl ]; then
  for zone in \
    /sys/class/powercap/intel-rapl:* \
    /sys/class/powercap/intel-rapl/intel-rapl:* \
    /sys/devices/virtual/powercap/intel-rapl/intel-rapl:*
  do
    [ -d "$zone" ] || continue
    [ -r "$zone/name" ] && [ -r "$zone/energy_uj" ] || continue
    zone_name=$(cat "$zone/name" 2>/dev/null)
    case "$zone_name" in package-*) ;; *) continue ;; esac
    energy=$(cat "$zone/energy_uj" 2>/dev/null)
    max_energy=$(cat "$zone/max_energy_range_uj" 2>/dev/null)
    [ -n "$energy" ] || continue
    [ -n "$max_energy" ] || max_energy=0
    printf 'RAPL|%s|%s|%s\n' "$zone_name" "$energy" "$max_energy"
  done
fi

if command -v sensors >/dev/null 2>&1; then
  sensors -u 2>/dev/null | awk '
    /^Adapter:/ { next }
    /^[[:space:]]*[^[:space:]].*:[[:space:]]*$/ {
      label=$0; sub(/^[[:space:]]+/, "", label); sub(/:[[:space:]]*$/, "", label); next
    }
    /^[^[:space:]]/ { chip=$0; label=""; next }
    /temp[0-9]+_input:/ { print "TEMP|" chip "|" label "|" $2 }
    /fan[0-9]+_input:/  { print "FAN|" chip "|" label "|" $2 }
    /power[0-9]+_(average|input):/ { print "POWER|" chip "|" label "|" $2 }
  '
fi

awk '/MemTotal:/ { total=$2*1024 } /MemAvailable:/ { available=$2*1024 }
     END { printf "MEM|%.0f|%.0f\n", total-available, total }' /proc/meminfo 2>/dev/null

df -P -B1 / 2>/dev/null | awk 'NR==2 { printf "DISK|%s|%s|%s\n", $6, $3, $2 }'

iface=$(ip route show default 2>/dev/null | awk 'NR==1 {print $5}')
if [ -z "$iface" ]; then
  iface=$(awk -F: 'NR>2 {gsub(/ /,"",$1); if ($1!="lo") {print $1; exit}}' /proc/net/dev)
fi
if [ -n "$iface" ] && [ -r "/sys/class/net/$iface/statistics/rx_bytes" ]; then
  rx=$(cat "/sys/class/net/$iface/statistics/rx_bytes")
  tx=$(cat "/sys/class/net/$iface/statistics/tx_bytes")
  printf 'NET|%s|%s|%s\n' "$iface" "$rx" "$tx"
fi

if command -v nvidia-smi >/dev/null 2>&1; then
  gpu_data=$(nvidia-smi --query-gpu=index,name,temperature.gpu,fan.speed,memory.used,memory.total,power.draw \
    --format=csv,noheader,nounits 2>/dev/null)
  if [ $? -ne 0 ]; then
    gpu_data=$(nvidia-smi --query-gpu=index,name,temperature.gpu,fan.speed,memory.used,memory.total \
      --format=csv,noheader,nounits 2>/dev/null)
  fi
  printf '%s\n' "$gpu_data" | while IFS= read -r line; do
    if [ -n "$line" ]; then
      printf 'GPU|%s\n' "$line"
    fi
  done
fi
printf 'END|1\n'
)SH";
    return script;
}

TelemetrySnapshot TelemetryParser::parse(const std::string& text, double latencyMs) {
    TelemetrySnapshot snapshot;
    snapshot.connected = true;
    snapshot.status = "Connected";
    snapshot.sshLatencyMs = latencyMs;
    snapshot.sampledAt = std::chrono::system_clock::now();

    bool hasNetwork = false;
    std::string networkInterface;
    std::uint64_t rx = 0;
    std::uint64_t tx = 0;
    std::unordered_map<std::string, RaplReading> raplReadings;
    std::stringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        auto parts = split(line, '|');
        if (parts.empty()) continue;
        if (parts[0] == "CPU" && parts.size() >= 3) {
            snapshot.cpuName = parts[1];
            snapshot.cpuPowerStrategy = parts[2];
        } else if (parts[0] == "RAPL" && parts.size() >= 4) {
            raplReadings[parts[1]] = {u64(parts[2]), u64(parts[3])};
        } else if (parts[0] == "TEMP" && parts.size() >= 4) {
            const auto labelLower = lower(parts[2]);
            const double value = real(parts[3]);
            if (value < -100.0 || value > 250.0) continue;
            if (isGpuChip(parts[1])) {
                auto gpu = std::find_if(snapshot.gpus.begin(), snapshot.gpus.end(), [&](const GpuMetrics& item) {
                    return item.name == parts[1];
                });
                if (gpu == snapshot.gpus.end()) {
                    snapshot.gpus.push_back(GpuMetrics{parts[1]});
                    gpu = std::prev(snapshot.gpus.end());
                }
                if (gpu->temperature < 0.0 || labelLower.find("edge") != std::string::npos)
                    gpu->temperature = value;
                continue;
            }
            if (labelLower.find("package") != std::string::npos ||
                labelLower == "tctl" || labelLower == "tdie" || labelLower == "die") {
                snapshot.packageTemperature = value;
            } else if (labelLower.find("core") != std::string::npos ||
                       labelLower.find("ccd") != std::string::npos) {
                snapshot.coreTemperatures.push_back({parts[2], value});
                if (snapshot.packageTemperature < 0.0) snapshot.packageTemperature = value;
            } else if (isCpuChip(parts[1]) && snapshot.packageTemperature < 0.0) {
                snapshot.packageTemperature = value;
            }
        } else if (parts[0] == "POWER" && parts.size() >= 4) {
            const double value = real(parts[3]);
            if (value < 0.0 || value > 10000.0) continue;
            if (isGpuChip(parts[1])) {
                auto gpu = std::find_if(snapshot.gpus.begin(), snapshot.gpus.end(), [&](const GpuMetrics& item) {
                    return item.name == parts[1];
                });
                if (gpu == snapshot.gpus.end()) {
                    snapshot.gpus.push_back(GpuMetrics{parts[1]});
                    gpu = std::prev(snapshot.gpus.end());
                }
                gpu->powerWatts = std::max(gpu->powerWatts, value);
            } else if (isCpuChip(parts[1])) {
                snapshot.packagePowerWatts = std::max(snapshot.packagePowerWatts, value);
            }
        } else if (parts[0] == "FAN" && parts.size() >= 4) {
            const double value = real(parts[3]);
            if (value <= 0.0) continue;
            if (isGpuChip(parts[1])) {
                auto gpu = std::find_if(snapshot.gpus.begin(), snapshot.gpus.end(), [&](const GpuMetrics& item) {
                    return item.name == parts[1];
                });
                if (gpu == snapshot.gpus.end()) {
                    snapshot.gpus.push_back(GpuMetrics{parts[1]});
                    gpu = std::prev(snapshot.gpus.end());
                }
                gpu->fanRpm = value;
            } else {
                snapshot.fans.push_back({parts[2].empty() ? parts[1] : parts[2], value});
            }
        } else if (parts[0] == "MEM" && parts.size() >= 3) {
            snapshot.memoryUsed = u64(parts[1]);
            snapshot.memoryTotal = u64(parts[2]);
        } else if (parts[0] == "DISK" && parts.size() >= 4) {
            snapshot.diskUsed = u64(parts[2]);
            snapshot.diskTotal = u64(parts[3]);
        } else if (parts[0] == "NET" && parts.size() >= 4) {
            auto readCounter = [](const std::string& text, std::uint64_t& value) {
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
                return ec == std::errc{} && end == text.data() + text.size();
            };
            hasNetwork = !parts[1].empty() && readCounter(parts[2], rx) && readCounter(parts[3], tx);
            networkInterface = parts[1];
        } else if (parts[0] == "GPU" && parts.size() >= 2) {
            auto fields = split(line.substr(4), ',');
            if (fields.size() >= 6) {
                GpuMetrics gpu;
                gpu.name = fields[1];
                gpu.temperature = real(fields[2]);
                gpu.fanPercent = real(fields[3]);
                gpu.memoryUsed = u64(fields[4]) * 1024ULL * 1024ULL;
                gpu.memoryTotal = u64(fields[5]) * 1024ULL * 1024ULL;
                if (fields.size() >= 7) gpu.powerWatts = real(fields[6]);
                snapshot.gpus.push_back(std::move(gpu));
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (!raplReadings.empty()) {
        if (previousRaplSample_.time_since_epoch().count() != 0) {
            const double seconds = std::chrono::duration<double>(now - previousRaplSample_).count();
            double totalWatts = 0.0;
            bool hasPower = false;
            if (seconds > 0.0) {
                for (const auto& [zone, current] : raplReadings) {
                    const auto previous = previousRaplReadings_.find(zone);
                    if (previous == previousRaplReadings_.end()) continue;

                    std::uint64_t delta = 0;
                    if (current.energyMicrojoules >= previous->second.energyMicrojoules) {
                        delta = current.energyMicrojoules - previous->second.energyMicrojoules;
                    } else {
                        const auto range = current.maxEnergyRangeMicrojoules != 0
                            ? current.maxEnergyRangeMicrojoules
                            : previous->second.maxEnergyRangeMicrojoules;
                        if (range <= previous->second.energyMicrojoules) continue;
                        delta = range - previous->second.energyMicrojoules + current.energyMicrojoules;
                    }
                    totalWatts += static_cast<double>(delta) / 1'000'000.0 / seconds;
                    hasPower = true;
                }
            }
            if (hasPower && totalWatts >= 0.0 && totalWatts <= 10000.0)
                snapshot.packagePowerWatts = totalWatts;
        }
        previousRaplReadings_ = std::move(raplReadings);
        previousRaplSample_ = now;
    }

    if (hasNetwork && networkInterface == previousInterface_ &&
        previousNetworkSample_.time_since_epoch().count() != 0 && rx >= previousRx_ && tx >= previousTx_) {
        const double seconds = std::chrono::duration<double>(now - previousNetworkSample_).count();
        if (seconds > 0.0) {
            snapshot.networkDown = static_cast<std::uint64_t>((rx - previousRx_) / seconds);
            snapshot.networkUp = static_cast<std::uint64_t>((tx - previousTx_) / seconds);
        }
    }
    previousRx_ = rx;
    previousTx_ = tx;
    previousInterface_ = hasNetwork ? networkInterface : std::string{};
    previousNetworkSample_ = hasNetwork ? now : std::chrono::steady_clock::time_point{};
    return snapshot;
}
