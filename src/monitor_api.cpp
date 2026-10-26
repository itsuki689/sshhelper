#include "monitor_api.hpp"
#include "credential_store.hpp"
#include "local_telemetry.hpp"
#include "ssh_client.hpp"
#include "telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace monitor_api {
namespace {
using Clock = std::chrono::steady_clock;
std::string timestamp(std::chrono::system_clock::time_point value) {
    const auto time = std::chrono::system_clock::to_time_t(value);
    std::tm utc{};
    if (gmtime_s(&utc, &time) != 0) return {};
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}
Json available(double value) {
    return std::isfinite(value) && value >= 0 ? Json(value) : Json(nullptr);
}
Json positive(std::uint64_t value) { return value ? Json(value) : Json(nullptr); }
Json sensors(const std::vector<SensorValue>& values) {
    auto result = Json::array();
    for (const auto& item : values) result.push_back({{"label", item.label}, {"value", available(item.value)}});
    return result;
}
Json capacity(std::uint64_t used, std::uint64_t total, bool valid) {
    return {{"used_bytes", valid ? Json(used) : Json(nullptr)},
            {"total_bytes", valid ? Json(total) : Json(nullptr)},
            {"used_percent", valid && total ? Json(100.0 * double(used) / double(total)) : Json(nullptr)}};
}
Json metrics(const Reading& reading, bool local) {
    const auto& s = reading.snapshot;
    if (!s.connected) return nullptr;
    Json gpus = Json::array();
    for (const auto& gpu : s.gpus) {
        gpus.push_back({{"name", gpu.name}, {"temperature_c", available(gpu.temperature)},
            {"power_w", available(gpu.powerWatts)}, {"fan_percent", available(gpu.fanPercent)},
            {"fan_rpm", available(gpu.fanRpm)}, {"memory_clock_mhz", available(gpu.memoryClockMHz)},
            {"memory", capacity(gpu.memoryUsed, gpu.memoryTotal, gpu.memoryTotal > 0)}});
    }
    return {{"cpu", {{"name", s.cpuName}, {"usage_percent", reading.ratesReady ? available(s.cpuUsagePercent) : Json(nullptr)},
                {"clock_mhz", available(s.cpuClockMHz)}, {"physical_cores", positive(s.cpuPhysicalCores)},
                {"logical_processors", positive(s.cpuLogicalProcessors)}, {"temperature_c", available(s.packageTemperature)},
                {"thermal_zone_c", available(s.thermalZoneTemperature)}, {"power_w", available(s.packagePowerWatts)},
                {"core_temperatures_c", sensors(s.coreTemperatures)}, {"chiplet_temperatures_c", sensors(s.chipletTemperatures)}}},
            {"gpus", std::move(gpus)}, {"fans_rpm", sensors(s.fans)},
            {"memory", capacity(s.memoryUsed, s.memoryTotal, s.memoryTotal > 0)},
            {"memory_configured_rate_mts", positive(s.memoryRateMTs)},
            {"swap", capacity(s.swapUsed, s.swapTotal, s.swapAvailable)},
            {"disk", capacity(s.diskUsed, s.diskTotal, s.diskTotal > 0)},
            {"ssd_temperature_c", available(s.ssdTemperature)},
            {"network", {{"down_bytes_per_second", reading.ratesReady && s.networkRatesAvailable ? Json(s.networkDown) : Json(nullptr)},
                         {"up_bytes_per_second", reading.ratesReady && s.networkRatesAvailable ? Json(s.networkUp) : Json(nullptr)},
                         {"link_mbps", positive(s.networkLinkMbps)}}},
            {"ssh_latency_ms", local ? Json(nullptr) : available(s.sshLatencyMs)}};
}
bool sameProfile(const ServerProfile& a, const ServerProfile& b) {
    const auto& x = a.connection; const auto& y = b.connection;
    return a.local == b.local && a.name == b.name && a.refreshIntervalSeconds == b.refreshIntervalSeconds &&
        x.host == y.host && x.port == y.port && x.username == y.username && x.password == y.password &&
        x.privateKeyPath == y.privateKeyPath && x.keyPassphrase == y.keyPassphrase;
}
class LocalSource final : public Source {
    LocalTelemetry sampler_;
    unsigned count_ = 0;
public:
    Reading sample(std::stop_token) override { return {sampler_.sample(), ++count_ >= 2}; }
};
class RemoteSource final : public Source {
    ConnectionConfig config_;
    std::filesystem::path pins_;
    std::unique_ptr<SshSession> session_;
    TelemetryParser parser_;
    unsigned count_ = 0;
public:
    RemoteSource(ConnectionConfig config, std::filesystem::path pins) : config_(std::move(config)), pins_(std::move(pins)) {}
    Reading sample(std::stop_token stop) override {
        std::string error, output;
        double latency = 0;
        if (!session_) {
            parser_ = {}; count_ = 0;
            session_ = std::make_unique<SshSession>(stop, 15000, pins_);
            if (!session_->connect(config_, error)) session_.reset();
        }
        if (session_ && session_->executeScript(TelemetryParser::probeScript(), output, latency, error)) {
            auto snapshot = parser_.parse(output, latency);
            return {std::move(snapshot), ++count_ >= 2};
        }
        session_.reset(); count_ = 0;
        TelemetrySnapshot failed;
        failed.status = error; failed.sampledAt = std::chrono::system_clock::now();
        return {std::move(failed), false};
    }
};
struct Worker {
    const ServerProfile profile;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<Reading> latest;
    Clock::time_point sampleStarted{};
    std::uint64_t requested = 0;
    // Declared last so the worker joins before its synchronization state is destroyed.
    std::jthread thread;

    Worker(ServerProfile value, SourceFactory factory) : profile(std::move(value)) {
        thread = std::jthread([this, factory = std::move(factory)](std::stop_token stop) {
            std::unique_ptr<Source> source;
            while (!stop.stop_requested()) {
                std::uint64_t request;
                { std::lock_guard lock(mutex); request = requested; }
                const auto started = Clock::now();
                Reading value;
                try {
                    if (!source) source = factory(profile);
                    if (!source) throw std::runtime_error("Monitor source unavailable");
                    value = source->sample(stop);
                } catch (const std::exception& error) {
                    value.snapshot.status = error.what();
                    value.snapshot.sampledAt = std::chrono::system_clock::now();
                    source.reset();
                }
                if (stop.stop_requested()) break;
                const auto delay = value.snapshot.connected && !value.ratesReady ? std::chrono::milliseconds(250) :
                    std::chrono::milliseconds(value.snapshot.connected ? profile.refreshIntervalSeconds * 1000 : 3000);
                std::unique_lock lock(mutex);
                latest = std::move(value); sampleStarted = started;
                changed.notify_all();
                changed.wait_for(lock, stop, delay, [&] { return requested != request; });
            }
        });
    }
    void stop() { thread.request_stop(); changed.notify_all(); }
    ~Worker() { stop(); }
    void refresh() { std::lock_guard lock(mutex); ++requested; changed.notify_all(); }
    void wait(bool fresh, Clock::time_point after, Clock::time_point deadline) {
        std::unique_lock lock(mutex);
        changed.wait_until(lock, deadline, [&] {
            return latest && (!fresh || (sampleStarted >= after &&
                (!latest->snapshot.connected || latest->ratesReady)));
        });
    }
    Json read(const std::string& id, Clock::time_point after) {
        std::lock_guard lock(mutex);
        const bool current = latest && sampleStarted >= after;
        Json result{{"server_id", id}, {"name", profile.name}, {"local", profile.local},
            {"host", profile.local ? Json(nullptr) : Json(profile.connection.host)},
            {"port", profile.local ? Json(nullptr) : Json(profile.connection.port)},
            {"refresh_interval_seconds", profile.refreshIntervalSeconds},
            {"is_fresh", current}, {"rates_ready", latest && latest->ratesReady}};
        if (!latest) {
            result.update({{"state", "pending"}, {"connected", false}, {"status", "Sampling did not finish within wait_seconds"},
                {"host_name", nullptr}, {"sampled_at", nullptr}, {"age_ms", nullptr}, {"metrics", nullptr}});
        } else {
            const auto& s = latest->snapshot;
            const auto age = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now() - s.sampledAt).count());
            result.update({{"state", s.connected ? "online" : "offline"}, {"connected", s.connected},
                {"status", s.status}, {"host_name", s.hostName}, {"sampled_at", timestamp(s.sampledAt)},
                {"age_ms", age}, {"metrics", metrics(*latest, profile.local)}});
        }
        return result;
    }
};
}

struct Service::Impl {
    std::filesystem::path credentials;
    SourceFactory factory;
    std::map<std::string, ServerProfile> profiles;
    std::map<std::string, std::unique_ptr<Worker>> workers;
    Impl(std::filesystem::path path, SourceFactory source) : credentials(std::move(path)), factory(std::move(source)) {
        if (!factory) {
            const auto pins = credentials.parent_path() / L"AirSFTP.known_hosts";
            factory = [pins](const ServerProfile& profile) -> std::unique_ptr<Source> {
                if (profile.local) return std::make_unique<LocalSource>();
                return std::make_unique<RemoteSource>(profile.connection, pins);
            };
        }
    }
    ~Impl() { for (auto& [id, worker] : workers) worker->stop(); }
    void reload() {
        ServerSettings settings;
        std::string error;
        if (!loadServerSettings(credentials, settings, error)) throw std::runtime_error(error);
        std::map<std::string, ServerProfile> loaded;
        unsigned remote = 0;
        for (auto& profile : settings.servers) {
            const auto id = profile.local ? "local" : "server-" + std::to_string(++remote);
            loaded.emplace(id, std::move(profile));
        }
        if (!loaded.contains("local")) {
            ServerProfile local; local.local = true; local.name = "本机";
            loaded.emplace("local", std::move(local));
        }
        for (auto& [id, worker] : workers)
            if (!loaded.contains(id) || !sameProfile(worker->profile, loaded.at(id))) worker->stop();
        std::erase_if(workers, [&](const auto& item) {
            return !loaded.contains(item.first) || !sameProfile(item.second->profile, loaded.at(item.first));
        });
        profiles = std::move(loaded);
    }
};
Service::Service(std::filesystem::path path, SourceFactory factory)
    : impl_(std::make_unique<Impl>(std::move(path), std::move(factory))) {}
Service::~Service() = default;

Json Service::listServers() {
    impl_->reload();
    Json servers = Json::array();
    for (const auto& [id, profile] : impl_->profiles)
        servers.push_back({{"server_id", id}, {"name", profile.name}, {"local", profile.local},
            {"host", profile.local ? Json(nullptr) : Json(profile.connection.host)},
            {"port", profile.local ? Json(nullptr) : Json(profile.connection.port)},
            {"refresh_interval_seconds", profile.refreshIntervalSeconds}});
    return {{"schema_version", 1}, {"servers", std::move(servers)}};
}
Json Service::getMonitoring(const Json& arguments) {
    if (!arguments.is_object()) throw std::invalid_argument("arguments must be an object");
    for (const auto& [key, value] : arguments.items())
        if (key != "server" && key != "fresh" && key != "wait_seconds") throw std::invalid_argument("Unknown argument: " + key);
    if ((arguments.contains("server") && !arguments["server"].is_string()) ||
        (arguments.contains("fresh") && !arguments["fresh"].is_boolean()) ||
        (arguments.contains("wait_seconds") && !arguments["wait_seconds"].is_number_integer()))
        throw std::invalid_argument("Invalid server, fresh or wait_seconds type");
    const auto seconds = arguments.value("wait_seconds", std::int64_t{20});
    if (seconds < 0 || seconds > 30) throw std::invalid_argument("wait_seconds must be between 0 and 30");
    const auto selector = arguments.value("server", std::string{});
    const bool fresh = arguments.value("fresh", true);
    impl_->reload();
    std::vector<std::string> ids;
    if (!selector.empty() && impl_->profiles.contains(selector)) ids.push_back(selector);
    else for (const auto& [id, profile] : impl_->profiles)
        if (selector.empty() || selector == profile.name ||
            (!profile.local && selector == profile.connection.host)) ids.push_back(id);
    if (ids.empty()) throw std::invalid_argument("Server not found. Call list_servers to get server_id values.");
    if (!selector.empty() && ids.size() != 1) throw std::invalid_argument("Ambiguous server. Use its server_id from list_servers.");
    const auto after = Clock::now();
    const auto deadline = after + std::chrono::seconds(seconds);
    for (const auto& id : ids) {
        auto& worker = impl_->workers[id];
        if (!worker) worker = std::make_unique<Worker>(impl_->profiles.at(id), impl_->factory);
        if (fresh) worker->refresh();
    }
    // Wait first, then collect every snapshot. A slow/offline host must not make
    // the already-collected local snapshot (and its age_ms) obsolete in transit.
    for (const auto& id : ids) impl_->workers.at(id)->wait(fresh, after, deadline);
    Json servers = Json::array();
    for (const auto& id : ids) servers.push_back(impl_->workers.at(id)->read(id, after));
    return {{"schema_version", 1}, {"requested_fresh", fresh}, {"generated_at", timestamp(std::chrono::system_clock::now())},
        {"servers", std::move(servers)}};
}
std::string serialize(const Json& value) { return value.dump(-1, ' ', false, Json::error_handler_t::replace); }
}
