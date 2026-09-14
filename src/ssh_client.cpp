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

bool verifyOrRememberHost(const ConnectionConfig& config, const std::string& fingerprint, std::string& error) {
    const auto path = knownHostsPath();
    const std::string key = config.host + ":" + std::to_string(config.port) + " ";
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind(key, 0) != 0) continue;
        const std::string known = line.substr(key.size());
        if (known == fingerprint) return true;
        error = "SSH host key changed for " + config.host + ". Expected " + known + ", received " + fingerprint;
        return false;
    }
    input.close();
    std::ofstream output(path, std::ios::app);
    if (!output) {
        error = "Cannot write portable host key store: " + path.string();
        return false;
    }
    output << key << fingerprint << std::endl;
    if (!output) {
        error = "Cannot save SSH host fingerprint";
        return false;
    }
    return true;
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

SshSession::SshSession(std::stop_token stop, long timeoutMs)
    : stop_(stop), timeoutMs_(std::max(1L, timeoutMs)) {}

SshSession::~SshSession() { disconnect(); }

bool SshSession::connect(const ConnectionConfig& config, std::string& error) {
    disconnect();
    error.clear();
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
    if (stop_.stop_requested() || fingerprint_.empty() || !verifyOrRememberHost(config, fingerprint_, error)) {
        if (stop_.stop_requested()) error = "Connection cancelled";
        disconnect();
        return false;
    }
    if (!authenticate(session_, config, error) || stop_.stop_requested()) {
        if (stop_.stop_requested()) error = "Connection cancelled";
        disconnect();
        return false;
    }
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

bool SshSession::transfer(TransferDirection direction,
                          const std::string& localPath,
                          const std::string& remotePath,
                          const std::function<void(const TransferProgress&)>& progress,
                          std::string& error) {
    error.clear();
    TransferProgress state;
    state.active = true;
    state.direction = direction;
    state.fileName = fileName(direction == TransferDirection::Upload ? localPath : remotePath);
    const auto started = std::chrono::steady_clock::now();
    auto lastReport = started - std::chrono::seconds(1);
    auto report = [&](bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastReport < std::chrono::milliseconds(100)) return;
        const auto elapsed = std::chrono::duration<double>(now - started).count();
        state.bytesPerSecond = elapsed > 0.0 ? static_cast<std::uint64_t>(state.transferred / elapsed) : 0;
        lastReport = now;
        progress(state);
    };
    auto finish = [&](bool ok) {
        if (stop_.stop_requested()) { error = "Transfer cancelled"; ok = false; }
        state.active = false;
        state.completed = ok;
        state.failed = !ok;
        state.message = ok ? "Completed" : error;
        report(true);
        return ok;
    };
    if (stop_.stop_requested()) return finish(false);
    if (!session_) { error = "SSH session is not connected"; return finish(false); }
    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session_);
    if (!sftp) { error = sessionError("Cannot initialize SFTP"); return finish(false); }

    bool ok = true;
    bool totalKnown = false;
    auto fail = [&](const char* message) {
        if (ok) error = message; // Preserve the first failure through cleanup.
        ok = false;
    };
    auto closeRemote = [&](LIBSSH2_SFTP_HANDLE* remote) {
        if (libssh2_sftp_close(remote) != 0) fail("Cannot close remote file");
    };
    if (direction == TransferDirection::Upload) {
        std::ifstream local(std::filesystem::u8path(localPath), std::ios::binary);
        if (!local) {
            fail("Cannot open local file");
        } else {
            local.seekg(0, std::ios::end);
            const auto size = local.tellg();
            if (size < std::streampos(0)) fail("Cannot determine local file size");
            else { state.total = static_cast<std::uint64_t>(size); totalKnown = true; }
            local.seekg(0, std::ios::beg);
            if (!local) fail("Cannot seek local file");
            // Validate the source before truncating the destination.
            if (ok && !stop_.stop_requested()) {
                LIBSSH2_SFTP_HANDLE* remote = libssh2_sftp_open(
                    sftp, remotePath.c_str(), LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
                if (!remote) {
                    fail("Cannot open remote destination");
                } else {
                    std::array<char, 64 * 1024> buffer{};
                    while (ok && !stop_.stop_requested()) {
                        local.read(buffer.data(), buffer.size());
                        if (local.bad() || (local.fail() && !local.eof())) {
                            fail("Cannot read local source");
                            break;
                        }
                        auto remaining = local.gcount();
                        const char* cursor = buffer.data();
                        while (remaining > 0 && !stop_.stop_requested()) {
                            const auto written = libssh2_sftp_write(remote, cursor, static_cast<std::size_t>(remaining));
                            if (written <= 0) { fail("SFTP upload failed"); break; }
                            cursor += written;
                            remaining -= written;
                            state.transferred += static_cast<std::uint64_t>(written);
                            report();
                        }
                        if (local.eof()) break;
                    }
                    closeRemote(remote);
                }
            }
        }
    } else {
        LIBSSH2_SFTP_HANDLE* remote = stop_.stop_requested() ? nullptr :
            libssh2_sftp_open(sftp, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
        if (!remote) {
            fail("Cannot open remote source");
        } else {
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            if (libssh2_sftp_fstat(remote, &attributes) == 0 && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
                state.total = attributes.filesize;
                totalKnown = true;
            }
            if (!stop_.stop_requested()) {
                std::ofstream local(std::filesystem::u8path(localPath), std::ios::binary | std::ios::trunc);
                if (!local) {
                    fail("Cannot open local destination");
                } else {
                    std::array<char, 64 * 1024> buffer{};
                    while (!stop_.stop_requested()) {
                        const auto count = libssh2_sftp_read(remote, buffer.data(), buffer.size());
                        if (count > 0) {
                            local.write(buffer.data(), count);
                            if (!local) { fail("Cannot write local destination"); break; }
                            state.transferred += static_cast<std::uint64_t>(count);
                            report();
                        } else if (count == 0) {
                            break;
                        } else {
                            fail("SFTP download failed");
                            break;
                        }
                    }
                    // Buffered writes can fail only at flush/close, after the last write succeeded.
                    local.flush();
                    if (!local) fail("Cannot flush local destination");
                    local.close();
                    if (!local) fail("Cannot close local destination");
                }
            }
            closeRemote(remote);
        }
    }
    if (ok && totalKnown && state.transferred != state.total)
        fail("Transferred size does not match source size");
    if (libssh2_sftp_shutdown(sftp) != 0) fail("Cannot close SFTP session");
    return finish(ok);
}
