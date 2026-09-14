#include "ssh_client.hpp"

#include <winsock2.h>
#include <windows.h>
#include <libssh2.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

// Exercise the exact file-opening implementation used for libssh2 private keys.
extern "C" FILE* ssh2_fopen(const char* filename, const char* mode);

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    return __LINE__; } } while (false)

class SilentServer {
public:
    SilentServer() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ == INVALID_SOCKET ||
            bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, 1) != 0) {
            if (listener_ != INVALID_SOCKET) closesocket(listener_);
            throw std::runtime_error("Cannot start loopback test server");
        }
        int size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token stop) {
            SOCKET peer = INVALID_SOCKET;
            while (!stop.stop_requested()) {
                const auto socket = peer == INVALID_SOCKET ? listener_ : peer;
                fd_set readable;
                FD_ZERO(&readable); FD_SET(socket, &readable);
                timeval interval{0, 50000};
                const int ready = select(0, &readable, nullptr, nullptr, &interval);
                if (ready < 0) break;
                if (!ready) continue;
                if (peer == INVALID_SOCKET) peer = accept(listener_, nullptr, nullptr);
                else {
                    char buffer[512];
                    if (recv(peer, buffer, sizeof(buffer), 0) <= 0) break;
                }
            }
            if (peer != INVALID_SOCKET) closesocket(peer);
        });
    }
    ~SilentServer() {
        worker_.request_stop();
        worker_.join();
        closesocket(listener_);
    }
    int port = 0;
private:
    SOCKET listener_ = INVALID_SOCKET;
    std::jthread worker_;
};

int runTests() {
    ConnectionConfig config;
    config.host = "127.0.0.1";
    config.username = "test";
    std::string error;
    {
        SilentServer server;
        config.port = server.port;
        SshSession session({}, 300);
        const auto started = std::chrono::steady_clock::now();
        CHECK(!session.connect(config, error));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed < std::chrono::seconds(2));
        CHECK(!session.connected());
        CHECK(!error.empty());
        std::cout << "Handshake timeout: "
                  << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
    }
    {
        SilentServer server;
        config.port = server.port;
        std::stop_source stop;
        SshSession session(stop.get_token(), 10000);
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
        const auto started = std::chrono::steady_clock::now();
        CHECK(!session.connect(config, error));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed < std::chrono::seconds(2));
        CHECK(error == "Connection cancelled");
        std::cout << "Handshake cancellation: "
                  << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
    }
    {
        std::stop_source stop;
        stop.request_stop();
        SshSession session(stop.get_token());
        config.host = "should-not-be-resolved.invalid";
        CHECK(!session.connect(config, error));
        CHECK(error == "Connection cancelled");
    }
    {
        SshSession session;
        TransferProgress final;
        CHECK(!session.transfer(TransferDirection::Upload, "unused", "/unused",
            [&](const TransferProgress& value) { final = value; }, error));
        CHECK(final.failed && !final.completed && !final.active);
        CHECK(!final.message.empty());
    }
    const auto path = std::filesystem::temp_directory_path() /
        (L"AirSFTP-key-\u4e2d\u6587-\u03bb-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    { std::ofstream file(path, std::ios::binary); file << "key-file-test"; CHECK(file.good()); }
    const auto encoded = path.u8string();
    const std::string utf8(encoded.begin(), encoded.end());
    FILE* file = ssh2_fopen(utf8.c_str(), "rb");
    const bool opened = file != nullptr;
    int first = EOF;
    if (file) { first = fgetc(file); fclose(file); }
    std::filesystem::remove(path);
    CHECK(opened && first == 'k');
    std::cout << "Unicode private-key path: OK\n";
    return 0;
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    if (libssh2_init(0) != 0) { WSACleanup(); return 1; }
    const int result = runTests();
    libssh2_exit();
    WSACleanup();
    return result;
}
