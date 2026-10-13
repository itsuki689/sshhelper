#include "credential_store.hpp"
#include "ssh_client.hpp"
#include "telemetry.hpp"
#include <winsock2.h>
#include <libssh2.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool good, const std::string& message) {
    if (!good) throw std::runtime_error(message);
}
int run(int argc, char** argv) {
    require(argc == 3, "Usage: live_telemetry_test HOST CREDENTIAL_FILE");
    ServerSettings saved;
    std::string error;
    require(loadServerSettings(fs::u8path(argv[2]), saved, error), error);
    const auto found = std::find_if(saved.servers.begin(), saved.servers.end(),
        [&](const auto& profile) { return profile.connection.host == argv[1]; });
    require(found != saved.servers.end(), "No saved profile for the specified host");
    const auto pins = fs::u8path(argv[2]).parent_path() / "AirSFTP.known_hosts";
    const auto testPins = defaultCredentialPath().parent_path() / "AirSFTP.known_hosts";
    require(fs::exists(pins), "Existing application host-key pins required");
    if (!fs::equivalent(pins.parent_path(), testPins.parent_path())) {
        std::ifstream input(pins, std::ios::binary);
        std::ofstream output(testPins, std::ios::binary | std::ios::trunc);
        require(input.good() && output.good(), "Cannot open host-key pins");
        output << input.rdbuf();
        output.close();
        require(!input.bad() && !output.fail(), "Cannot copy host-key pins");
    }
    SshSession session;
    require(session.connect(found->connection, error), "Connect: " + error);
    std::string output;
    double latency = 0;
    const std::string diagnostics = R"SH(
printf '%s\n' '--- CPU ---'
awk -F: '/^model name/ {print $2; exit}' /proc/cpuinfo
printf '%s\n' '--- sensors ---'
command -v sensors || true
sensors -u 2>&1 || true
printf '%s\n' '--- hwmon ---'
for dir in /sys/class/hwmon/hwmon*; do
  [ -d "$dir" ] || continue
  printf '%s: ' "$dir"; cat "$dir/name" 2>/dev/null
  for input in "$dir"/temp*_input; do
    [ -e "$input" ] || continue
    printf '%s ' "$input"
    cat "${input%_input}_label" 2>/dev/null
    cat "$input" 2>&1
  done
done
printf '%s\n' '--- thermal ---'
for dir in /sys/class/thermal/thermal_zone*; do
  [ -d "$dir" ] || continue
  cat "$dir/type" "$dir/temp" 2>&1
done
printf '%s\n' '--- coretemp module ---'
ls -ld /sys/module/coretemp 2>&1 || true
)SH";
    require(session.executeScript(diagnostics, output, latency, error), error);
    std::cout << output << std::endl;
    require(session.executeScript(TelemetryParser::probeScript(), output, latency, error), error);
    std::cout << "--- probe ---\n" << output;
    TelemetryParser parser;
    const auto snapshot = parser.parse(output, latency);
    std::cout << "Parsed CPU temperature: " << snapshot.packageTemperature
              << " C; core count: " << snapshot.coreTemperatures.size() << std::endl;
    std::cout << "Negotiated link speed: " << snapshot.networkLinkMbps << " Mbps" << std::endl;
    std::cout << "SSD temperature: " << snapshot.ssdTemperature << " C" << std::endl;
    if (!snapshot.gpus.empty())
        std::cout << "GPU fan: " << snapshot.gpus[0].fanPercent << " %" << std::endl;
    std::cout << "Configured memory rate: " << snapshot.memoryRateMTs << " MT/s" << std::endl;
    if (!snapshot.gpus.empty())
        std::cout << "GPU memory clock: " << snapshot.gpus[0].memoryClockMHz << " MHz; VRAM used: "
                  << snapshot.gpus[0].memoryUsed << "/" << snapshot.gpus[0].memoryTotal << " bytes" << std::endl;
    require(snapshot.packageTemperature >= 0, "CPU temperature unavailable");
    return 0;
}
int main(int argc, char** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    if (libssh2_init(0)) { WSACleanup(); return 1; }
    int result = 1;
    try { result = run(argc, argv); }
    catch (const std::exception& e) { std::cerr << e.what() << std::endl; }
    libssh2_exit();
    WSACleanup();
    return result;
}
