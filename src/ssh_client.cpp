#include "ssh_client.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
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

SshSession::~SshSession() { disconnect(); }

bool SshSession::connect(const ConnectionConfig& config, std::string& error) {
    disconnect();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto port = std::to_string(config.port);
    const int lookup = getaddrinfo(config.host.c_str(), port.c_str(), &hints, &addresses);
    if (lookup != 0) {
        error = "Cannot resolve host: " + config.host;
        return false;
    }

    SOCKET connectedSocket = INVALID_SOCKET;
    for (auto* address = addresses; address; address = address->ai_next) {
        SOCKET candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        DWORD timeout = 15000;
        setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            connectedSocket = candidate;
            break;
        }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);
    if (connectedSocket == INVALID_SOCKET) {
        error = socketMessage(WSAGetLastError());
        return false;
    }
    socket_ = static_cast<std::uintptr_t>(connectedSocket);

    session_ = libssh2_session_init();
    if (!session_) {
        error = "Cannot allocate SSH session";
        disconnect();
        return false;
    }
    libssh2_session_set_blocking(session_, 1);
    if (libssh2_session_handshake(session_, connectedSocket) != 0) {
        error = sessionError("SSH handshake failed");
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

    if (fingerprint_.empty() || !verifyOrRememberHost(config, fingerprint_, error)) {
        disconnect();
        return false;
    }

    if (!authenticate(session_, config, error)) {
        disconnect();
        return false;
    }
    return true;
}

void SshSession::disconnect() {
    if (session_) {
        libssh2_session_disconnect(session_, "AirSFTP closed");
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
    while (sent < script.size()) {
        const auto written = libssh2_channel_write(channel, script.data() + sent, script.size() - sent);
        if (written < 0) {
            error = sessionError("Cannot send remote probe");
            libssh2_channel_free(channel);
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    libssh2_channel_send_eof(channel);

    std::array<char, 8192> buffer{};
    bool receivedFirstByte = false;
    for (;;) {
        const auto count = libssh2_channel_read(channel, buffer.data(), buffer.size());
        if (count > 0) {
            if (!receivedFirstByte) {
                receivedFirstByte = true;
                firstByteLatencyMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
            }
            output.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else {
            error = sessionError("Cannot read remote probe");
            libssh2_channel_free(channel);
            return false;
        }
    }
    libssh2_channel_close(channel);
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
                          const std::atomic_bool& cancelled,
                          std::string& error) {
    if (!session_) {
        error = "SSH session is not connected";
        return false;
    }
    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session_);
    if (!sftp) {
        error = sessionError("Cannot initialize SFTP");
        return false;
    }

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

    bool ok = true;
    if (direction == TransferDirection::Upload) {
        std::ifstream local(std::filesystem::u8path(localPath), std::ios::binary);
        if (!local) {
            error = "Cannot open local file";
            ok = false;
        } else {
            local.seekg(0, std::ios::end);
            state.total = static_cast<std::uint64_t>(local.tellg());
            local.seekg(0, std::ios::beg);
            LIBSSH2_SFTP_HANDLE* remote = libssh2_sftp_open(
                sftp, remotePath.c_str(), LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
            if (!remote) {
                error = "Cannot open remote destination";
                ok = false;
            } else {
                std::array<char, 64 * 1024> buffer{};
                while (local && !cancelled.load()) {
                    local.read(buffer.data(), buffer.size());
                    auto remaining = local.gcount();
                    char* cursor = buffer.data();
                    while (remaining > 0 && !cancelled.load()) {
                        const auto written = libssh2_sftp_write(remote, cursor, static_cast<std::size_t>(remaining));
                        if (written < 0) { error = "SFTP upload failed"; ok = false; break; }
                        cursor += written;
                        remaining -= written;
                        state.transferred += static_cast<std::uint64_t>(written);
                        report();
                    }
                    if (!ok) break;
                }
                libssh2_sftp_close(remote);
            }
        }
    } else {
        LIBSSH2_SFTP_HANDLE* remote = libssh2_sftp_open(sftp, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
        if (!remote) {
            error = "Cannot open remote source";
            ok = false;
        } else {
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            if (libssh2_sftp_fstat(remote, &attributes) == 0 && (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE))
                state.total = attributes.filesize;
            std::ofstream local(std::filesystem::u8path(localPath), std::ios::binary | std::ios::trunc);
            if (!local) {
                error = "Cannot open local destination";
                ok = false;
            } else {
                std::array<char, 64 * 1024> buffer{};
                while (!cancelled.load()) {
                    const auto count = libssh2_sftp_read(remote, buffer.data(), buffer.size());
                    if (count > 0) {
                        local.write(buffer.data(), count);
                        if (!local) { error = "Cannot write local destination"; ok = false; break; }
                        state.transferred += static_cast<std::uint64_t>(count);
                        report();
                    } else if (count == 0) {
                        break;
                    } else {
                        error = "SFTP download failed";
                        ok = false;
                        break;
                    }
                }
            }
            libssh2_sftp_close(remote);
        }
    }

    libssh2_sftp_shutdown(sftp);
    if (cancelled.load()) {
        error = "Transfer cancelled";
        ok = false;
    }
    state.active = false;
    state.completed = ok;
    state.failed = !ok;
    state.message = ok ? "Completed" : error;
    report(true);
    return ok;
}
