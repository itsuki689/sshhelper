#pragma once
// Real loopback TCP/DNS and SshSession flow; only the SSH peer is stubbed.
#include "ssh_client.hpp"
#include <winsock2.h>
#include <windows.h>
#include <libssh2.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <stdexcept>
#include <thread>

inline std::atomic<unsigned> sshHandshakes{0}, sshAuthentications{0};
inline thread_local std::array<char, 32> sshTestHash{};
inline std::filesystem::path expectedPinStore;
inline std::atomic<bool> authenticationWithoutPin{false};
extern "C" {
int __wrap_libssh2_session_handshake(LIBSSH2_SESSION*, libssh2_socket_t) { ++sshHandshakes; return 0; }
const char* __wrap_libssh2_hostkey_hash(LIBSSH2_SESSION*, int) { return sshTestHash.data(); }
int __wrap_libssh2_userauth_password_ex(LIBSSH2_SESSION*, const char*, unsigned int,
    const char*, unsigned int, LIBSSH2_PASSWD_CHANGEREQ_FUNC(*)) {
    ++sshAuthentications;
    if (!expectedPinStore.empty() && !std::filesystem::exists(expectedPinStore)) authenticationWithoutPin = true;
    return 0;
}
}
class LoopbackSsh {
public:
    ConnectionConfig config;
    LoopbackSsh() {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
            listen(listener_, SOMAXCONN)) throw std::runtime_error("Cannot listen on loopback");
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        config.host = "127.0.0.1"; config.port = ntohs(address.sin_port); config.username = "test";
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                fd_set ready; FD_ZERO(&ready); FD_SET(listener_, &ready);
                timeval interval{0, 10000};
                if (select(0, &ready, nullptr, nullptr, &interval) > 0) {
                    auto peer = accept(listener_, nullptr, nullptr);
                    if (peer != INVALID_SOCKET) closesocket(peer);
                }
            }
        });
    }
    ~LoopbackSsh() { worker_.request_stop(); worker_.join(); closesocket(listener_); }
private:
    SOCKET listener_ = INVALID_SOCKET;
    std::jthread worker_;
};

template<class F> bool eventually(F condition, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}
inline std::string readPins(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
