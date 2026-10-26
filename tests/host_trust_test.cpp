#include "mock_ssh.hpp"
#include "credential_store.hpp"
#include "monitor_api.hpp"
#include "mcp_server.hpp"
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n" << error << "\n"; return __LINE__; } } while(false)
using namespace std::chrono_literals;

int runTests(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::string error;
    LoopbackSsh server;
    const auto pins = root / "AirSFTP.known_hosts";
    expectedPinStore = pins;
    unsigned prompts = 0;
    auto accept = [&](const HostKey& key, const auto&) {
        ++prompts;
        if (key.host != server.config.host || key.port != server.config.port || fs::exists(pins))
            return HostTrustResult::Rejected;
        return HostTrustResult::Accepted;
    };
    {
        SshSession session({}, 15000, pins, accept);
        CHECK(session.connect(server.config, error));
        CHECK(prompts == 1 && sshAuthentications == 1 && !authenticationWithoutPin);
    }
    const auto saved = readPins(pins);
    CHECK(saved.find("SHA256 00:00:") != std::string::npos); // Legacy disk format retained.
    CHECK(displayHostFingerprint("SHA256 " + std::string(saved.substr(saved.find("SHA256 ") + 7, 95))) ==
        "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    {
        SshSession known({}, 15000, pins, accept);
        CHECK(known.connect(server.config, error));
        CHECK(prompts == 1 && sshAuthentications == 2 && readPins(pins) == saved);
        sshTestHash[0] = 1;
        CHECK(!known.connect(server.config, error));
        CHECK(error.find("host key changed") != std::string::npos && known.requiresUserAction());
        CHECK(prompts == 1 && sshAuthentications == 2 && readPins(pins) == saved);
        sshTestHash[0] = 0;
    }
    fs::remove(pins);
    {
        SshSession rejected({}, 15000, pins, [](const auto&, const auto&) { return HostTrustResult::Rejected; });
        CHECK(!rejected.connect(server.config, error) && rejected.requiresUserAction());
        CHECK(error.find("rejected") != std::string::npos && sshAuthentications == 2 && !fs::exists(pins));
        SshSession deniedByDefault({}, 15000, pins);
        CHECK(!deniedByDefault.connect(server.config, error) && error.find("not pinned") != std::string::npos);
        CHECK(!fs::exists(pins) && sshAuthentications == 2);
    }
    {
        SshSession saveFailure({}, 15000, root / "absent/keys", [](const auto&, const auto&) { return HostTrustResult::Accepted; });
        CHECK(!saveFailure.connect(server.config, error) && saveFailure.requiresUserAction());
        CHECK(error.find("Cannot save") != std::string::npos && sshAuthentications == 2);
        // Replacement failure preserves existing records byte for byte.
        std::ofstream(pins) << "other:22 SHA256 original\r\n";
        const auto original = readPins(pins);
        HANDLE held = CreateFileW(pins.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);
        SshSession replaceFailure({}, 15000, pins, [](const auto&, const auto&) { return HostTrustResult::Accepted; });
        const auto connected = replaceFailure.connect(server.config, error);
        CloseHandle(held);
        CHECK(!connected && sshAuthentications == 2 && readPins(pins) == original);
        fs::remove(pins);
    }
    {
        // A record installed while a prompt is open must be rechecked, never overwritten.
        SshSession conflicting({}, 15000, pins, [&](const HostKey& key, const auto&) {
            std::ofstream(pins) << key.host << ':' << key.port << " changed-during-confirmation\n";
            return HostTrustResult::Accepted;
        });
        CHECK(!conflicting.connect(server.config, error));
        CHECK(error.find("host key changed") != std::string::npos && sshAuthentications == 2);
        fs::remove(pins);
    }
    {
        std::stop_source stop;
        SshSession late({}, 15000, pins, [](const auto&, const auto& attempt) {
            attempt->cancel(); return HostTrustResult::Accepted;
        });
        CHECK(!late.connect(server.config, error) && !fs::exists(pins) && sshAuthentications == 2);
        SshSession cancelled(stop.get_token(), 15000, pins, [&](const auto&, const auto&) {
            stop.request_stop(); return HostTrustResult::Accepted;
        });
        CHECK(!cancelled.connect(server.config, error) && !fs::exists(pins) && sshAuthentications == 2);
    }
    {
        HostTrustCoordinator coordinator;
        std::atomic<unsigned> entered{0};
        auto confirmation = [&](const HostKey& key, const auto& attempt) {
            ++entered; return coordinator.confirm(key, attempt, &server);
        };
        auto connect = [&] {
            SshSession session({}, 15000, pins, confirmation);
            std::string localError;
            return session.connect(server.config, localError);
        };
        auto first = std::async(std::launch::async, connect);
        auto second = std::async(std::launch::async, connect);
        CHECK(eventually([&] {
            const auto pending = coordinator.pending();
            return entered == 2 && pending.size() == 1 && pending[0]->waiting == 2;
        }));
        // Lock inspection and saving an unrelated host remain possible during the wait.
        HostKey other{"independent", 2200, "SHA256 independent"};
        HostTrustAttempt otherAttempt({});
        CHECK(saveHostKey(pins, other, {}, otherAttempt, error));
        CHECK(sshAuthentications == 2);
        auto request = coordinator.pending()[0];
        CHECK(coordinator.resolve(request, HostTrustResult::Accepted));
        CHECK(first.get() && second.get());
        CHECK(!coordinator.resolve(request, HostTrustResult::Accepted));
        CHECK(sshAuthentications == 4 && readPins(pins).find("independent:2200") != std::string::npos);
        CHECK(readPins(pins).find(server.config.host + ":" + std::to_string(server.config.port)) != std::string::npos);
        fs::remove(pins);
    }
    {
        HostTrustCoordinator coordinator;
        auto confirmation = [&](const HostKey& key, const auto& attempt) { return coordinator.confirm(key, attempt, &server); };
        auto connect = [&](char hash) {
            sshTestHash[0] = hash;
            SshSession session({}, 15000, pins, confirmation);
            std::string localError;
            return session.connect(server.config, localError);
        };
        auto first = std::async(std::launch::async, connect, 1);
        auto second = std::async(std::launch::async, connect, 2);
        CHECK(eventually([&] { return coordinator.pending().size() == 2; }));
        const auto requests = coordinator.pending();
        CHECK(requests[0]->key.fingerprint != requests[1]->key.fingerprint);
        CHECK(coordinator.resolve(requests[0], HostTrustResult::Accepted));
        CHECK(eventually([&] { return fs::exists(pins); }));
        CHECK(coordinator.resolve(requests[1], HostTrustResult::Accepted));
        const bool a = first.get(), b = second.get();
        CHECK(a != b && sshAuthentications == 5); // At most one key can be pinned/authenticated.
        fs::remove(pins);
    }
    for (int scenario = 0; scenario != 4; ++scenario) {
        HostTrustCoordinator coordinator;
        std::stop_source stop;
        auto connect = std::async(std::launch::async, [&] {
            SshSession session(stop.get_token(), 15000, pins,
                [&](const HostKey& key, const auto& attempt) { return coordinator.confirm(key, attempt, &server); },
                scenario == 0 ? 300ms : 5s);
            std::string localError;
            const auto connected = session.connect(server.config, localError);
            return std::pair(connected, localError);
        });
        CHECK(eventually([&] { return coordinator.pending().size() == 1; }));
        const auto stale = coordinator.pending()[0];
        if (scenario == 1) stop.request_stop(); // Task cancellation / configuration replacement.
        if (scenario == 2) coordinator.cancelOwner(&server); // Hide / profile deletion.
        if (scenario == 3) coordinator.shutdown(); // Application exit.
        const auto result = connect.get();
        CHECK(!result.first && !fs::exists(pins) && sshAuthentications == 5);
        CHECK(result.second.find(scenario == 0 ? "timed out" : "cancelled") != std::string::npos);
        CHECK(!coordinator.resolve(stale, HostTrustResult::Accepted) && coordinator.pending().empty());
    }
    {
        // Cancelling one subscriber must preserve the other subscriber's prompt.
        HostTrustCoordinator coordinator;
        std::stop_source stop;
        auto firstAttempt = std::make_shared<HostTrustAttempt>(stop.get_token());
        auto secondAttempt = std::make_shared<HostTrustAttempt>(std::stop_token{});
        const HostKey key{"shared", 22, "SHA256 shared"};
        auto first = std::async(std::launch::async, [&] { return coordinator.confirm(key, firstAttempt, &server); });
        auto second = std::async(std::launch::async, [&] { return coordinator.confirm(key, secondAttempt, &pins); });
        CHECK(eventually([&] {
            auto pending = coordinator.pending(); return pending.size() == 1 && pending[0]->waiting == 2;
        }));
        stop.request_stop();
        CHECK(first.get() == HostTrustResult::Cancelled);
        CHECK(coordinator.pending().size() == 1);
        CHECK(coordinator.resolve(coordinator.pending()[0], HostTrustResult::Accepted));
        CHECK(second.get() == HostTrustResult::Accepted);
    }
    {
        // Concurrent saves must preserve all unrelated records, including CRLF data.
        std::ofstream(pins, std::ios::binary) << "legacy:2222 SHA256 original\r\n";
        std::vector<std::future<bool>> saves;
        for (int i = 0; i != 12; ++i) saves.push_back(std::async(std::launch::async, [&, i] {
            HostTrustAttempt attempt({});
            std::string saveError;
            return saveHostKey(pins, {"concurrent-" + std::to_string(i), 22, "SHA256 test"}, {}, attempt, saveError);
        }));
        for (auto& save : saves) CHECK(save.get());
        const auto contents = readPins(pins);
        CHECK(contents.starts_with("legacy:2222 SHA256 original\r\n"));
        for (int i = 0; i != 12; ++i)
            CHECK(contents.find("concurrent-" + std::to_string(i) + ":22 SHA256 test\n") != std::string::npos);
        fs::remove(pins);
    }
    {
        // End-to-end MCP uses the production RemoteSource and unknown-key policy.
        ServerSettings settings;
        ServerProfile remote; remote.name = "loopback"; remote.connection = server.config;
        settings.servers.push_back(remote);
        const auto credentials = root / "AirSFTP.credentials";
        CHECK(saveServerSettings(credentials, settings, error));
        monitor_api::Service service(credentials);
        sshhelper_mcp::Server mcp([&](const std::string& name, const auto& args) {
            return name == "list_servers" ? service.listServers() : service.getMonitoring(args);
        });
        using monitor_api::Json;
        CHECK(mcp.handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
            {"params", {{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
            {"clientInfo", {{"name", "test"}, {"version", "1"}}}}}})->contains("result"));
        for (bool existing : {false, true}) {
            if (existing) std::ofstream(pins) << "other:22 existing-pin\n";
            const auto original = readPins(pins);
            for (int i = 0; i != 2; ++i) {
                auto response = mcp.handle({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                    {"params", {{"name", "get_monitoring"}, {"arguments", {{"server", "server-1"},
                    {"fresh", true}, {"wait_seconds", 2}}}}}});
                const auto value = response->at("result")["structuredContent"]["servers"][0];
                CHECK(value["connected"] == false && value["status"].get<std::string>().find("not pinned") != std::string::npos);
                CHECK(fs::exists(pins) == existing && readPins(pins) == original && sshAuthentications == 5);
            }
        }
        fs::remove(pins);
    }
    std::cout << "Host trust acceptance, persistence, concurrency, cancellation and production MCP policy passed\n";
    return 0;
}
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) || libssh2_init(0)) return 1;
    const auto root = std::filesystem::temp_directory_path() / ("airsftp-trust-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directory(root);
    const int result = runTests(root);
    std::filesystem::remove_all(root);
    libssh2_exit(); WSACleanup();
    return result;
}
