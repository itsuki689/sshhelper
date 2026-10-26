#include "host_trust.hpp"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>

HostTrustAttempt::HostTrustAttempt(std::stop_token stop, std::chrono::milliseconds timeout)
    : stop_(stop), deadline_(std::chrono::steady_clock::now() + timeout),
      cancellation_(stop, [this] { cancel(); }) {}
void HostTrustAttempt::cancel() { std::lock_guard lock(mutex_); cancelled_ = true; }
HostTrustResult HostTrustAttempt::state() const {
    std::lock_guard lock(mutex_);
    if (cancelled_ || stop_.stop_requested()) return HostTrustResult::Cancelled;
    if (std::chrono::steady_clock::now() >= deadline_) return HostTrustResult::TimedOut;
    return HostTrustResult::Pending;
}
bool HostTrustAttempt::commit(const std::function<bool()>& action) {
    std::lock_guard lock(mutex_);
    if (cancelled_ || stop_.stop_requested() || std::chrono::steady_clock::now() >= deadline_) return false;
    return action();
}

bool HostTrustCoordinator::live(const Request& request) const {
    return std::any_of(request.attempts.begin(), request.attempts.end(), [](const auto& weak) {
        auto attempt = weak.lock();
        return attempt && attempt->state() == HostTrustResult::Pending;
    });
}
HostTrustResult HostTrustCoordinator::confirm(const HostKey& key,
    const std::shared_ptr<HostTrustAttempt>& attempt, const void* owner) {
    std::unique_lock lock(mutex_);
    if (stopped_) return HostTrustResult::Cancelled;
    if (auto state = attempt->state(); state != HostTrustResult::Pending) return state;
    std::erase_if(requests_, [](const auto& weak) { return weak.expired(); });
    std::erase_if(participants_, [](const auto& p) { return p.attempt.expired(); });
    RequestPtr request;
    for (const auto& weak : requests_) {
        auto current = weak.lock();
        if (current && current->key == key && current->result == HostTrustResult::Pending && live(*current)) {
            request = current; break;
        }
    }
    if (!request) {
        request = RequestPtr(new Request(++nextId_, key));
        requests_.push_back(request);
    }
    request->attempts.push_back(attempt);
    participants_.push_back({owner, attempt});
    ++request->waiting;
    struct Leave { Request& request; ~Leave() { --request.waiting; } } leave{*request};
    for (;;) {
        if (stopped_) return HostTrustResult::Cancelled;
        if (auto state = attempt->state(); state != HostTrustResult::Pending) return state;
        if (request->result != HostTrustResult::Pending) return request->result;
        changed_.wait_for(lock, std::chrono::milliseconds(25));
    }
}
std::vector<HostTrustCoordinator::RequestPtr> HostTrustCoordinator::pending() {
    std::lock_guard lock(mutex_);
    std::vector<RequestPtr> result;
    if (!stopped_) for (const auto& weak : requests_)
        if (auto request = weak.lock(); request && request->result == HostTrustResult::Pending && live(*request))
            result.push_back(std::move(request));
    return result;
}
bool HostTrustCoordinator::resolve(const RequestPtr& request, HostTrustResult result) {
    std::lock_guard lock(mutex_);
    if (result == HostTrustResult::Pending || stopped_ || !request ||
        request->result != HostTrustResult::Pending || !live(*request)) return false;
    // Reject foreign requests, including a request from a previous coordinator.
    if (std::none_of(requests_.begin(), requests_.end(), [&](const auto& weak) { return weak.lock() == request; }))
        return false;
    request->result = result;
    changed_.notify_all();
    return true;
}
void HostTrustCoordinator::cancelOwner(const void* owner) {
    std::vector<std::shared_ptr<HostTrustAttempt>> attempts;
    {
        std::lock_guard lock(mutex_);
        for (const auto& p : participants_) if (p.owner == owner)
            if (auto attempt = p.attempt.lock()) attempts.push_back(std::move(attempt));
    }
    for (const auto& attempt : attempts) attempt->cancel();
    changed_.notify_all();
}
void HostTrustCoordinator::shutdown() {
    std::vector<std::shared_ptr<HostTrustAttempt>> attempts;
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        for (const auto& p : participants_)
            if (auto attempt = p.attempt.lock()) attempts.push_back(std::move(attempt));
    }
    for (const auto& attempt : attempts) attempt->cancel();
    changed_.notify_all();
}

std::string displayHostFingerprint(const std::string& fingerprint) {
    if (!fingerprint.starts_with("SHA256 ")) return fingerprint;
    std::istringstream input(fingerprint.substr(7));
    std::string field, bytes;
    while (std::getline(input, field, ':')) {
        if (field.size() != 2 || field.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return fingerprint;
        bytes.push_back(static_cast<char>(std::stoul(field, nullptr, 16)));
    }
    if (bytes.size() != 32) return fingerprint;
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output = "SHA256:";
    unsigned accumulator = 0; int bits = 0;
    for (unsigned char byte : bytes) {
        accumulator = (accumulator << 8) | byte; bits += 8;
        while (bits >= 6) { bits -= 6; output += alphabet[(accumulator >> bits) & 63]; }
    }
    if (bits) output += alphabet[(accumulator << (6 - bits)) & 63];
    return output;
}

namespace {
// Serialize readers/writers across application processes as well as threads.
// This kernel mutex never creates or changes the known_hosts file.
class StoreLock {
    HANDLE mutex_ = nullptr;
    bool acquired_ = false;
public:
    StoreLock(const std::filesystem::path& path, std::stop_token stop) {
        std::error_code ec;
        const auto normalized = std::filesystem::absolute(path, ec).lexically_normal().wstring();
        if (ec) return;
        std::uint64_t hash = 14695981039346656037ULL;
        for (auto c : normalized) { hash ^= static_cast<std::uint64_t>(std::towlower(c)); hash *= 1099511628211ULL; }
        const auto name = L"Local\\AirSFTP.HostKeys." + std::to_wstring(hash);
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            const auto result = WaitForSingleObject(mutex_, 25);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) { acquired_ = true; break; }
            if (result != WAIT_TIMEOUT) break;
        }
    }
    ~StoreLock() { if (acquired_) ReleaseMutex(mutex_); if (mutex_) CloseHandle(mutex_); }
    explicit operator bool() const { return acquired_; }
};
HostKeyStatus readStore(const std::filesystem::path& path, const HostKey& key,
                        std::string& contents, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) return HostKeyStatus::Unknown;
    if (ec || !std::filesystem::is_regular_file(path, ec)) {
        error = "Cannot read SSH host key store"; return HostKeyStatus::Error;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "Cannot read SSH host key store"; return HostKeyStatus::Error; }
    contents.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) { error = "Cannot read SSH host key store"; return HostKeyStatus::Error; }
    std::istringstream lines(contents);
    const auto prefix = key.host + ":" + std::to_string(key.port) + " ";
    std::string line;
    bool found = false;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.starts_with(prefix)) continue;
        if (line.substr(prefix.size()) != key.fingerprint) {
            error = "SSH host key changed for " + key.host + ":" + std::to_string(key.port) +
                ". Expected " + line.substr(prefix.size()) + ", received " + key.fingerprint;
            return HostKeyStatus::Changed;
        }
        found = true;
    }
    return found ? HostKeyStatus::Matches : HostKeyStatus::Unknown;
}
}
HostKeyStatus inspectHostKey(const std::filesystem::path& path, const HostKey& key,
                             std::stop_token stop, std::string& error) {
    StoreLock lock(path, stop);
    if (!lock) { error = "Cannot lock SSH host key store (cancelled or timed out)"; return HostKeyStatus::Error; }
    std::string contents;
    return readStore(path, key, contents, error);
}
bool saveHostKey(const std::filesystem::path& path, const HostKey& key,
                 std::stop_token stop, HostTrustAttempt& attempt, std::string& error) {
    StoreLock lock(path, stop);
    if (!lock) { error = "Cannot lock SSH host key store (cancelled or timed out)"; return false; }
    std::string contents;
    const auto status = readStore(path, key, contents, error);
    if (status == HostKeyStatus::Changed || status == HostKeyStatus::Error) return false;
    if (status == HostKeyStatus::Matches) {
        if (attempt.commit([] { return true; })) return true;
        error = "SSH host trust attempt cancelled or timed out";
        return false;
    }
    if (!contents.empty() && contents.back() != '\n') contents += '\n';
    contents += key.host + ":" + std::to_string(key.port) + " " + key.fingerprint + "\n";
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetCurrentThreadId());
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = "Cannot save SSH host fingerprint (temporary file)"; return false; }
    DWORD written = 0;
    const bool saved = contents.size() <= MAXDWORD &&
        WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
        written == contents.size() && FlushFileBuffers(file);
    DWORD commitError = saved ? ERROR_SUCCESS : GetLastError();
    const bool closed = CloseHandle(file) != FALSE;
    if (!closed) commitError = GetLastError();
    const bool committed = saved && closed && attempt.commit([&] {
        const bool moved = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!moved) commitError = GetLastError();
        return moved;
    });
    if (!committed) {
        DeleteFileW(temporary.c_str());
        error = "Cannot save SSH host fingerprint (cancelled, timed out or write failed; Windows error " +
            std::to_string(commitError) + ")";
    }
    return committed;
}
