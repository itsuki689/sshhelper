#include "monitor_api.hpp"
#include "credential_store.hpp"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n"; return __LINE__; } } while(false)
using namespace std::chrono_literals;
using monitor_api::Json;
struct FakeSource final : monitor_api::Source {
    ServerProfile profile;
    std::shared_ptr<std::atomic<unsigned>> calls;
    unsigned samples = 0;
    FakeSource(ServerProfile value, std::shared_ptr<std::atomic<unsigned>> count)
        : profile(std::move(value)), calls(std::move(count)) {}
    monitor_api::Reading sample(std::stop_token stop) override {
        const auto duration = profile.name == "slow" ? 1500ms : 10ms;
        const auto end = std::chrono::steady_clock::now() + duration;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(5ms);
        ++*calls;
        TelemetrySnapshot value;
        value.connected = profile.name != "offline";
        value.status = value.connected ? "Connected" : "Connection refused";
        value.sampledAt = std::chrono::system_clock::now();
        value.hostName = "测试主机";
        value.cpuUsagePercent = 12.5; value.packageTemperature = -1;
        value.networkDown = 2048; value.networkUp = 1024;
        value.networkRatesAvailable = profile.name != "missing-network";
        value.memoryUsed = 1024; value.memoryTotal = 4096;
        value.swapAvailable = true;
        value.gpus.push_back(GpuMetrics{"GPU", 42, 50, 0});
        return {std::move(value), ++samples >= 2};
    }
};
template<class F> bool rejects(F action) {
    try { action(); return false; } catch (const std::exception&) { return true; }
}
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("sshhelper-api-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{root};
    const auto path = root / "AirSFTP.credentials";
    const auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto factory = [calls](const ServerProfile& profile) { return std::make_unique<FakeSource>(profile, calls); };
    monitor_api::Service service(path, factory);
    CHECK(service.listServers()["servers"].size() == 1);
    CHECK(*calls == 0); // Discovery must never contact hosts.
    CHECK(rejects([&] { service.getMonitoring({{"server", "missing"}}); }));
    CHECK(rejects([&] { service.getMonitoring({{"fresh", "yes"}}); }));
    CHECK(rejects([&] { service.getMonitoring({{"wait_seconds", 31}}); }));
    CHECK(rejects([&] { service.getMonitoring({{"command", "whoami"}}); }));
    const auto first = service.getMonitoring({{"server", "local"}, {"wait_seconds", 2}})["servers"][0];
    CHECK(first["connected"] == true && first["is_fresh"] == true && first["rates_ready"] == true);
    CHECK(first["sampled_at"].is_string() && first["age_ms"].get<int>() < 500);
    CHECK(first["metrics"]["cpu"]["temperature_c"].is_null());
    CHECK(first["metrics"]["cpu"]["usage_percent"] == 12.5);
    CHECK(first["metrics"]["memory"]["used_percent"] == 25);
    CHECK(first["metrics"]["gpus"][0]["fan_percent"] == 0);
    CHECK(first["metrics"]["swap"]["used_bytes"] == 0);
    CHECK(first["metrics"]["network"]["down_bytes_per_second"] == 2048);
    CHECK(first["metrics"]["ssh_latency_ms"].is_null());
    const auto callCount = calls->load();
    service.getMonitoring({{"fresh", false}, {"wait_seconds", 0}});
    CHECK(calls->load() == callCount);
    service.getMonitoring({{"fresh", true}, {"wait_seconds", 2}});
    CHECK(calls->load() > callCount);

    ServerSettings settings;
    ServerProfile remote;
    remote.name = "远程测试"; remote.connection.host = "example.invalid";
    remote.connection.username = "private-user"; remote.connection.password = "secret-password";
    remote.connection.privateKeyPath = "secret-key-file"; remote.connection.keyPassphrase = "secret-passphrase";
    remote.refreshIntervalSeconds = 3600;
    settings.servers.push_back(remote);
    std::string error;
    CHECK(saveServerSettings(path, settings, error));
    const auto listed = monitor_api::serialize(service.listServers());
    CHECK(listed.find("secret-") == std::string::npos && listed.find("private-user") == std::string::npos);
    const auto remoteData = service.getMonitoring({{"server", "远程测试"}, {"wait_seconds", 2}});
    CHECK(remoteData["servers"].size() == 1 && remoteData["servers"][0]["server_id"] == "server-1");
    CHECK(monitor_api::serialize(remoteData).find("secret-") == std::string::npos);
    // Refresh interrupts a 3600-second configured interval.
    CHECK(service.getMonitoring({{"server", "server-1"}, {"wait_seconds", 2}})["servers"][0]["is_fresh"] == true);
    settings.servers.push_back(remote);
    settings.servers[1].name = "server-1"; // A display name cannot shadow an exact ID.
    CHECK(saveServerSettings(path, settings, error));
    CHECK(service.getMonitoring({{"server", "server-1"}, {"wait_seconds", 2}})["servers"].size() == 1);
    CHECK(rejects([&] { service.getMonitoring({{"server", "example.invalid"}}); }));
    settings.servers.resize(1);
    settings.servers[0].name = "missing-network";
    CHECK(saveServerSettings(path, settings, error));
    const auto missingNetwork = service.getMonitoring({{"server", "server-1"}, {"wait_seconds", 2}})["servers"][0];
    CHECK(missingNetwork["rates_ready"] == true);
    CHECK(missingNetwork["metrics"]["network"]["down_bytes_per_second"].is_null());
    settings.servers[0].name = "offline";
    settings.servers.resize(1);
    CHECK(saveServerSettings(path, settings, error));
    const auto offline = service.getMonitoring({{"server", "server-1"}, {"wait_seconds", 2}})["servers"][0];
    CHECK(offline["state"] == "offline" && offline["metrics"].is_null());
    settings.servers[0].name = "slow";
    settings.servers.push_back(settings.servers[0]);
    CHECK(saveServerSettings(path, settings, error));
    const auto begin = std::chrono::steady_clock::now();
    const auto pending = service.getMonitoring({{"wait_seconds", 1}});
    CHECK(std::chrono::steady_clock::now() - begin < 1400ms); // One deadline for all workers.
    CHECK(pending["servers"][1]["state"] == "pending" && pending["servers"][2]["is_fresh"] == false);
    CHECK(pending["servers"][1]["metrics"].is_null());
    CHECK(pending["servers"][0]["age_ms"].get<int>() >= 500); // Age is measured after waiting on slow hosts.
    std::ofstream(path, std::ios::trunc) << "corrupt";
    CHECK(rejects([&] { service.listServers(); })); // Never silently discard damaged credentials.
    std::cout << "Monitoring freshness, concurrency, selection, privacy and missing-data checks passed\n";
    return 0;
}
