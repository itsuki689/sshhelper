#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

struct HostKey {
    std::string host;
    int port = 22;
    // Keep the original SHA256 hex representation on disk.
    std::string fingerprint;
    bool operator==(const HostKey&) const = default;
};

std::string displayHostFingerprint(const std::string& fingerprint);
enum class HostTrustResult { Pending, Accepted, Rejected, Cancelled, TimedOut };

// One connection attempt. Cancellation and the final disk commit share a gate:
// after cancellation returns, this attempt cannot publish a new pin.
class HostTrustAttempt {
public:
    explicit HostTrustAttempt(std::stop_token stop,
        std::chrono::milliseconds timeout = std::chrono::seconds(120));
    void cancel();
    HostTrustResult state() const;
    bool commit(const std::function<bool()>& action);
private:
    std::stop_token stop_;
    std::chrono::steady_clock::time_point deadline_;
    mutable std::mutex mutex_;
    bool cancelled_ = false;
    std::stop_callback<std::function<void()>> cancellation_;
};
using HostTrustConfirmation = std::function<HostTrustResult(
    const HostKey&, const std::shared_ptr<HostTrustAttempt>&)>;

// No UI or disk operations under this coordinator's mutex. Only pending
// requests for the exact same host, port and fingerprint can be merged.
class HostTrustCoordinator {
public:
    struct Request {
        const std::uint64_t id;
        const HostKey key;
        std::atomic<std::size_t> waiting{0};
    private:
        friend class HostTrustCoordinator;
        Request(std::uint64_t value, HostKey host) : id(value), key(std::move(host)) {}
        HostTrustResult result = HostTrustResult::Pending;
        std::vector<std::weak_ptr<HostTrustAttempt>> attempts;
    };
    using RequestPtr = std::shared_ptr<Request>;
    HostTrustResult confirm(const HostKey& key, const std::shared_ptr<HostTrustAttempt>& attempt,
                            const void* owner);
    std::vector<RequestPtr> pending();
    bool resolve(const RequestPtr& request, HostTrustResult result);
    void cancelOwner(const void* owner);
    void shutdown();
private:
    bool live(const Request& request) const;
    struct Participant { const void* owner; std::weak_ptr<HostTrustAttempt> attempt; };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::weak_ptr<Request>> requests_;
    std::vector<Participant> participants_;
    std::uint64_t nextId_ = 0;
    bool stopped_ = false;
};

enum class HostKeyStatus { Unknown, Matches, Changed, Error };
HostKeyStatus inspectHostKey(const std::filesystem::path& path, const HostKey& key,
                             std::stop_token stop, std::string& error);
bool saveHostKey(const std::filesystem::path& path, const HostKey& key,
                 std::stop_token stop, HostTrustAttempt& attempt, std::string& error);
