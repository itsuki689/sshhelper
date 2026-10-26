#include "ssh_client.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
constexpr auto invalidSocket = static_cast<std::uintptr_t>(INVALID_SOCKET);

std::string socketMessage(int code) {
    return "Socket error " + std::to_string(code);
}

std::string fileName(const std::string& value) {
    const auto slash = value.find_last_of("/\\");
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

std::filesystem::path knownHostsPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
        return std::filesystem::path(buffer.data()).parent_path() / L"AirSFTP.known_hosts";
    return std::filesystem::current_path() / L"AirSFTP.known_hosts";
}

bool authenticate(LIBSSH2_SESSION* session, const ConnectionConfig& config, std::string& error) {
    int result = 0;
    if (!config.privateKeyPath.empty()) {
        result = libssh2_userauth_publickey_fromfile_ex(
            session,
            config.username.c_str(), static_cast<unsigned int>(config.username.size()),
            nullptr, config.privateKeyPath.c_str(),
            config.keyPassphrase.empty() ? nullptr : config.keyPassphrase.c_str());
    } else {
        result = libssh2_userauth_password_ex(
            session,
            config.username.c_str(), static_cast<unsigned int>(config.username.size()),
            config.password.c_str(), static_cast<unsigned int>(config.password.size()), nullptr);
    }
    if (result == 0) return true;
    error = "SSH authentication failed (" + std::to_string(result) + ")";
    return false;
}
}

SshSession::SshSession(std::stop_token stop, long timeoutMs,
                     std::filesystem::path hostKeyStore, HostTrustConfirmation confirm,
                     std::chrono::milliseconds trustTimeout)
    : stop_(stop), timeoutMs_(std::max(1L, timeoutMs)),
      hostKeyStore_(hostKeyStore.empty() ? knownHostsPath() : std::move(hostKeyStore)),
      confirmHost_(std::move(confirm)), trustTimeout_(trustTimeout) {}

SshSession::~SshSession() { disconnect(); }

bool SshSession::connect(const ConnectionConfig& supplied, std::string& error) {
    const ConnectionConfig config = supplied;
    disconnect();
    error.clear();
    requiresUserAction_ = false;
    if (stop_.stop_requested()) { error = "Connection cancelled"; return false; }

    // Older MinGW SDKs omit these declarations; Windows 8+ exports both APIs.
    using CancelLookup = INT (WSAAPI*)(LPHANDLE);
    using LookupResult = INT (WSAAPI*)(LPOVERLAPPED);
    const auto winsock = GetModuleHandleW(L"ws2_32.dll");
    const auto cancelLookup = reinterpret_cast<CancelLookup>(GetProcAddress(winsock, "GetAddrInfoExCancel"));
    const auto lookupResult = reinterpret_cast<LookupResult>(GetProcAddress(winsock, "GetAddrInfoExOverlappedResult"));
    if (!cancelLookup || !lookupResult) { error = "Asynchronous DNS is unavailable"; return false; }

    const int hostLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        config.host.c_str(), -1, nullptr, 0);
    if (!hostLength) { error = "Invalid host name"; return false; }
    std::wstring host(hostLength, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, config.host.c_str(), -1, host.data(), hostLength);
    const auto port = std::to_wstring(config.port);
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    PADDRINFOEXW addresses = nullptr;
    OVERLAPPED lookup{};
    lookup.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!lookup.hEvent) { error = "Cannot create DNS completion event"; return false; }
    HANDLE lookupHandle = nullptr;
    timeval timeout{timeoutMs_ / 1000, (timeoutMs_ % 1000) * 1000};
    int result = GetAddrInfoExW(host.c_str(), port.c_str(), NS_DNS, nullptr, &hints,
        &addresses, &timeout, &lookup, nullptr, &lookupHandle);
    if (result == WSA_IO_PENDING) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
        while (WaitForSingleObject(lookup.hEvent, 50) == WAIT_TIMEOUT) {
            if (stop_.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
                cancelLookup(&lookupHandle);
                // Stack storage must remain alive until the cancelled request completes.
                WaitForSingleObject(lookup.hEvent, INFINITE);
                break;
            }
        }
        result = lookupResult(&lookup);
    }
    CloseHandle(lookup.hEvent);
    if (result != 0 || stop_.stop_requested()) {
        if (addresses) FreeAddrInfoExW(addresses);
        error = stop_.stop_requested() ? "Connection cancelled" : "Cannot resolve host: " + config.host;
        return false;
    }

    SOCKET connectedSocket = INVALID_SOCKET;
    int socketError = WSAETIMEDOUT;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
    for (auto* address = addresses; address && !stop_.stop_requested();
         address = reinterpret_cast<PADDRINFOEXW>(address->ai_next)) {
        SOCKET candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) { socketError = WSAGetLastError(); continue; }
        u_long nonblocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) != 0) {
            socketError = WSAGetLastError();
            closesocket(candidate);
            continue;
        }
        bool ready = ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0;
        if (!ready) {
            socketError = WSAGetLastError();
            if (socketError == WSAEWOULDBLOCK || socketError == WSAEINPROGRESS) {
                while (!stop_.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                    fd_set writable, failed;
                    FD_ZERO(&writable); FD_ZERO(&failed);
                    FD_SET(candidate, &writable); FD_SET(candidate, &failed);
                    timeval interval{0, 50000};
                    const int selected = select(0, nullptr, &writable, &failed, &interval);
                    if (selected < 0) { socketError = WSAGetLastError(); break; }
                    if (selected == 0) { socketError = WSAETIMEDOUT; continue; }
                    int size = sizeof(socketError);
                    if (getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char*>(&socketError), &size) != 0)
                        socketError = WSAGetLastError();
                    ready = socketError == 0;
                    break;
                }
            }
        }
        if (ready) { connectedSocket = candidate; break; }
        closesocket(candidate);
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    FreeAddrInfoExW(addresses);
    if (connectedSocket == INVALID_SOCKET) {
        error = stop_.stop_requested() ? "Connection cancelled" : socketMessage(socketError);
        return false;
    }
    const int noDelay = 1;
    setsockopt(connectedSocket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    socket_ = static_cast<std::uintptr_t>(connectedSocket);
    cancellation_.emplace(stop_, [this] {
        std::lock_guard lock(socketMutex_);
        if (socket_ != invalidSocket) shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
    });
    if (stop_.stop_requested()) { error = "Connection cancelled"; disconnect(); return false; }

    session_ = libssh2_session_init();
    if (!session_) {
        error = "Cannot allocate SSH session";
        disconnect();
        return false;
    }
    libssh2_session_set_blocking(session_, 1);
    libssh2_session_set_timeout(session_, timeoutMs_);
    if (libssh2_session_handshake(session_, connectedSocket) != 0) {
        error = stop_.stop_requested() ? "Connection cancelled" : sessionError("SSH handshake failed");
        disconnect();
        return false;
    }
    if (const char* hash = libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256)) {
        std::ostringstream stream;
        stream << "SHA256 ";
        for (int index = 0; index < 32; ++index) {
            if (index) stream << ':';
            stream << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned int>(static_cast<unsigned char>(hash[index]));
        }
        fingerprint_ = stream.str();
    }
    auto attempt = std::make_shared<HostTrustAttempt>(stop_, trustTimeout_);
    const HostKey key{config.host, config.port, fingerprint_};
    auto failTrust = [&] {
        requiresUserAction_ = true;
        if (error.empty()) error = "SSH host trust attempt cancelled or timed out";
        if (stop_.stop_requested()) error = "Connection cancelled";
        disconnect();
        return false;
    };
    if (fingerprint_.empty()) { error = "SSH SHA256 host fingerprint unavailable"; return failTrust(); }
    const auto status = inspectHostKey(hostKeyStore_, key, stop_, error);
    if (status == HostKeyStatus::Changed || status == HostKeyStatus::Error) return failTrust();
    if (status == HostKeyStatus::Unknown) {
        if (!confirmHost_) {
            error = "SSH host key is not pinned. Explicitly verify and accept it in sshhelper before using sshhelper-tool.";
            return failTrust();
        }
        const auto answer = confirmHost_(key, attempt);
        if (answer != HostTrustResult::Accepted) {
            error = answer == HostTrustResult::TimedOut ? "SSH host trust confirmation timed out; connect manually to retry" :
                answer == HostTrustResult::Rejected ? "SSH host trust rejected; connect manually to retry" :
                "SSH host trust confirmation cancelled; connect manually to retry";
            return failTrust();
        }
        if (!saveHostKey(hostKeyStore_, key, stop_, *attempt, error)) return failTrust();
    }
    if (attempt->state() != HostTrustResult::Pending) {
        error = "SSH host trust attempt cancelled or timed out";
        return failTrust();
    }
    if (!authenticate(session_, config, error) || stop_.stop_requested()) {
        if (stop_.stop_requested()) error = "Connection cancelled";
        disconnect();
        return false;
    }
    serverIdentity_ = config.host + ":" + std::to_string(config.port) + ":" + config.username;
    return true;
}

void SshSession::disconnect() {
    // Wait for the cancellation callback before closing a potentially reused handle.
    cancellation_.reset();
    if (socket_ != invalidSocket) shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
    if (session_) {
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (socket_ != invalidSocket) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = invalidSocket;
    }
    fingerprint_.clear();
}

std::string SshSession::sessionError(const std::string& fallback) const {
    if (!session_) return fallback;
    char* message = nullptr;
    int length = 0;
    libssh2_session_last_error(session_, &message, &length, 0);
    if (message && length > 0) return std::string(message, static_cast<std::size_t>(length));
    return fallback;
}

bool SshSession::executeScript(const std::string& script, std::string& output,
                               double& firstByteLatencyMs, std::string& error) {
    output.clear();
    error.clear();
    if (stop_.stop_requested()) { error = "Probe cancelled"; return false; }
    firstByteLatencyMs = 0.0;
    if (!session_) {
        error = "SSH session is not connected";
        return false;
    }
    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session_);
    if (!channel) {
        error = sessionError("Cannot open SSH channel");
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    if (libssh2_channel_exec(channel, "LC_ALL=C sh -s") != 0) {
        error = sessionError("Cannot start remote probe");
        libssh2_channel_free(channel);
        return false;
    }

    std::size_t sent = 0;
    while (sent < script.size() && !stop_.stop_requested()) {
        const auto written = libssh2_channel_write(channel, script.data() + sent, script.size() - sent);
        if (written <= 0) {
            error = sessionError("Cannot send remote probe");
            libssh2_channel_free(channel);
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    if (stop_.stop_requested() || libssh2_channel_send_eof(channel) != 0) {
        error = stop_.stop_requested() ? "Probe cancelled" : sessionError("Cannot send probe EOF");
        libssh2_channel_free(channel);
        return false;
    }
    libssh2_channel_handle_extended_data2(channel, LIBSSH2_CHANNEL_EXTENDED_DATA_IGNORE);

    std::array<char, 8192> buffer{};
    bool receivedFirstByte = false;
    auto lastData = std::chrono::steady_clock::now();
    for (;;) {
        if (stop_.stop_requested() || std::chrono::steady_clock::now() - lastData >= std::chrono::milliseconds(timeoutMs_)) {
            error = stop_.stop_requested() ? "Probe cancelled" : "Remote probe timed out";
            libssh2_channel_free(channel);
            return false;
        }
        const auto count = libssh2_channel_read(channel, buffer.data(), buffer.size());
        if (count > 0) {
            lastData = std::chrono::steady_clock::now();
            if (!receivedFirstByte) {
                receivedFirstByte = true;
                firstByteLatencyMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            if (libssh2_channel_eof(channel)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            error = sessionError("Cannot read remote probe");
            libssh2_channel_free(channel);
            return false;
        }
    }
    if (libssh2_channel_close(channel) != 0) {
        error = sessionError("Cannot close remote probe");
        libssh2_channel_free(channel);
        return false;
    }
    const int exitStatus = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);
    if (exitStatus != 0) {
        error = "Remote probe exited with code " + std::to_string(exitStatus);
        return false;
    }
    return true;
}
