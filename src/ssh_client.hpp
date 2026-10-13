#pragma once

#include "models.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

struct _LIBSSH2_SESSION;
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;

struct _LIBSSH2_SFTP;
typedef struct _LIBSSH2_SFTP LIBSSH2_SFTP;
using TransferConfirmation = std::function<bool(std::size_t, std::uint64_t)>;

class SshSession {
public:
    explicit SshSession(std::stop_token stop = {}, long timeoutMs = 15000);
    ~SshSession();
    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    bool connect(const ConnectionConfig& config, std::string& error);
    void disconnect();
    bool connected() const { return session_ != nullptr; }
    const std::string& fingerprint() const { return fingerprint_; }

    bool executeScript(const std::string& script, std::string& output,
                       double& firstByteLatencyMs, std::string& error);

    // Lists regular files and directories recursively, without following links.
    bool listFolder(TransferDirection direction, const std::string& folder,
                    std::vector<TransferEntry>& entries, std::string& error);
    // nullopt transfers all contents; an explicit selection contains exact relative paths.
    bool transferFolder(TransferDirection direction, const std::string& localFolder,
                        const std::string& remoteFolder,
                        const std::optional<std::vector<std::string>>& selection,
                        const std::function<void(const TransferProgress&)>& progress,
                        std::string& error, const TransferConfirmation& confirm = {});

    bool uploadPaths(const std::vector<std::string>& paths, const std::string& remoteFolder,
                     const std::function<void(const TransferProgress&)>& progress, std::string& error,
                     const TransferConfirmation& confirm = {});

    bool transfer(TransferDirection direction,
                  const std::string& localPath,
                  const std::string& remotePath,
                  const std::function<void(const TransferProgress&)>& progress,
                  std::string& error);

private:
    bool transferItems(TransferDirection direction, const std::string& localFolder,
        const std::string& remoteFolder, const std::optional<std::vector<std::string>>& selection,
        const std::vector<std::string>* drops,
        const std::function<void(const TransferProgress&)>& progress, std::string& error,
        const TransferConfirmation& confirm);
    bool transferOnSftp(LIBSSH2_SFTP* sftp, TransferDirection direction, const std::string& localPath,
        const std::string& remotePath, const std::function<void(const TransferProgress&)>& progress,
        std::string& error, std::optional<std::uint64_t> expectedSize = std::nullopt);
    std::string sessionError(const std::string& fallback) const;

    std::stop_token stop_;
    long timeoutMs_;
    std::mutex socketMutex_;
    std::optional<std::stop_callback<std::function<void()>>> cancellation_;
    LIBSSH2_SESSION* session_ = nullptr;
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(~0ULL);
    std::string fingerprint_;
    std::string serverIdentity_;
};
