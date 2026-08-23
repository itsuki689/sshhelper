#pragma once

#include "models.hpp"

#include <functional>
#include <string>

struct _LIBSSH2_SESSION;
typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;

class SshSession {
public:
    SshSession() = default;
    ~SshSession();
    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    bool connect(const ConnectionConfig& config, std::string& error);
    void disconnect();
    bool connected() const { return session_ != nullptr; }
    const std::string& fingerprint() const { return fingerprint_; }

    bool executeScript(const std::string& script, std::string& output,
                       double& firstByteLatencyMs, std::string& error);

    bool transfer(TransferDirection direction,
                  const std::string& localPath,
                  const std::string& remotePath,
                  const std::function<void(const TransferProgress&)>& progress,
                  const std::atomic_bool& cancelled,
                  std::string& error);

private:
    std::string sessionError(const std::string& fallback) const;

    LIBSSH2_SESSION* session_ = nullptr;
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(~0ULL);
    std::string fingerprint_;
};
