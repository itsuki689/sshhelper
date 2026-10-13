#include "ssh_client.hpp"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Remote = std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)>;
struct Local {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Local() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void close() {
        const auto value = handle; handle = INVALID_HANDLE_VALUE;
        if (!CloseHandle(value)) throw std::runtime_error("Cannot close local file");
    }
};
struct TargetLock {
    HANDLE handle;
    explicit TargetLock(const std::wstring& name) : handle(CreateMutexW(nullptr, FALSE, name.c_str())) {
        if (!handle) throw std::runtime_error("Cannot lock transfer target");
        const auto result = WaitForSingleObject(handle, 0);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            CloseHandle(handle); handle = nullptr;
            throw std::runtime_error("Another transfer is using this destination");
        }
    }
    ~TargetLock() { if (handle) { ReleaseMutex(handle); CloseHandle(handle); } }
};
std::string digest(const std::string& value) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize transfer identity hash");
    std::array<unsigned char, 32> bytes{};
    const auto result = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())), ULONG(value.size()), bytes.data(), ULONG(bytes.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) throw std::runtime_error("Cannot hash transfer identity");
    std::string text;
    for (auto byte : bytes) { text += "0123456789abcdef"[byte >> 4]; text += "0123456789abcdef"[byte & 15]; }
    return text;
}
std::string encoded(const fs::path& path) {
    const auto text = path.generic_u8string(); return {text.begin(), text.end()};
}
void position(HANDLE file, std::uint64_t offset) {
    LARGE_INTEGER value; value.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, value, nullptr, FILE_BEGIN)) throw std::runtime_error("Cannot seek local file");
}
std::uint64_t sizeOf(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) throw std::runtime_error("Cannot read local file size");
    return static_cast<std::uint64_t>(size.QuadPart);
}
void checkLocalPath(const fs::path& path) {
    auto full = fs::absolute(path).lexically_normal();
    auto current = full.root_path();
    for (const auto& part : full.relative_path()) {
        current /= part;
        const auto flags = GetFileAttributesW(current.c_str());
        if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Local links/junctions are not supported");
    }
}
bool remoteStat(LIBSSH2_SFTP* sftp, const std::string& path, LIBSSH2_SFTP_ATTRIBUTES& value) {
    value = {};
    if (libssh2_sftp_lstat(sftp, path.c_str(), &value) == 0) {
        if (!(value.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISREG(value.permissions))
            throw std::runtime_error("Remote path is not a regular file: " + path);
        if (!(value.flags & LIBSSH2_SFTP_ATTR_SIZE)) throw std::runtime_error("Remote file size unavailable");
        return true;
    }
    if (libssh2_sftp_last_error(sftp) != LIBSSH2_FX_NO_SUCH_FILE)
        throw std::runtime_error("Cannot inspect remote file: " + path);
    return false;
}
LIBSSH2_SFTP_ATTRIBUTES remoteInfo(LIBSSH2_SFTP_HANDLE* handle) {
    LIBSSH2_SFTP_ATTRIBUTES value{};
    if (libssh2_sftp_fstat(handle, &value) != 0 || !(value.flags & LIBSSH2_SFTP_ATTR_SIZE))
        throw std::runtime_error("Cannot read remote file size");
    return value;
}
}

bool SshSession::transfer(TransferDirection direction, const std::string& localPath,
    const std::string& remotePath, const std::function<void(const TransferProgress&)>& progress, std::string& error) {
    TransferProgress last; last.direction = direction;
    if (!session_ || stop_.stop_requested()) {
        error = stop_.stop_requested() ? "Transfer cancelled" : "SSH session is not connected";
        last.failed = true; last.message = error; progress(last); return false;
    }
    auto* sftp = libssh2_sftp_init(session_);
    if (!sftp) {
        error = "Cannot initialize SFTP"; last.failed = true; last.message = error; progress(last); return false;
    }
    bool ok = transferOnSftp(sftp, direction, localPath, remotePath,
        [&](const TransferProgress& value) { last = value; if (!value.completed) progress(value); }, error);
    if (libssh2_sftp_shutdown(sftp) != 0 && ok) { error = "Cannot close SFTP session"; ok = false; }
    last.active = false; last.completed = ok; last.failed = !ok;
    if (!ok) last.message = error;
    progress(last); return ok;
}

bool SshSession::transferOnSftp(LIBSSH2_SFTP* sftp, TransferDirection direction, const std::string& localPath,
    const std::string& remotePath, const std::function<void(const TransferProgress&)>& progress,
    std::string& error, std::optional<std::uint64_t> expectedSize) {
    TransferProgress state; state.active = true; state.direction = direction;
    state.fileName = encoded(fs::u8path(localPath).filename());
    auto started = std::chrono::steady_clock::now();
    auto lastReport = started - std::chrono::seconds(1);
    auto report = [&](bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastReport < std::chrono::milliseconds(100)) return;
        const auto elapsed = std::chrono::duration<double>(now - started).count();
        state.bytesPerSecond = elapsed > 0 ? std::uint64_t((state.transferred - state.resumedBytes) / elapsed) : 0;
        lastReport = now; progress(state);
    };
    auto cancelled = [&] { if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled"); };
    error.clear();
    try {
        cancelled();
        const bool upload = direction == TransferDirection::Upload;
        const auto local = fs::absolute(fs::u8path(localPath)).lexically_normal();
        checkLocalPath(local);
        auto localKey = local.wstring();
        CharLowerBuffW(localKey.data(), DWORD(localKey.size()));
        const auto identity = serverIdentity_ + "\n" + remotePath + "\n" + encoded(fs::path(localKey));
        const auto targetKey = digest(upload ? serverIdentity_ + "\n" + remotePath : encoded(fs::path(localKey)));
        TargetLock targetLock(L"Local\\AirSFTP-transfer-" + std::wstring(targetKey.begin(), targetKey.end()));
        const auto temporaryName = ".airsftp-" + digest(identity) + ".part";
        const auto localTemporary = local.parent_path() / fs::u8path(temporaryName);
        const auto slash = remotePath.find_last_of('/');
        if (slash == std::string::npos || remotePath.front() != '/') throw std::runtime_error("Remote path must be absolute");
        const auto remoteTemporary = remotePath.substr(0, slash + 1) + temporaryName;
        LIBSSH2_SFTP_ATTRIBUTES attr{}, sourceAttr{};
        Local localFile;
        Remote remoteFile(nullptr, libssh2_sftp_close_handle);
        std::uint64_t offset = 0;
        if (upload) {
            localFile.handle = CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (localFile.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open local source");
            state.total = sizeOf(localFile.handle);
            const bool targetExists = remoteStat(sftp, remotePath, attr); // Never opened for writing.
            const long mode = targetExists ? long(attr.permissions & 0777) : 0644;
            const bool exists = remoteStat(sftp, remoteTemporary, attr);
            remoteFile.reset(libssh2_sftp_open(sftp, remoteTemporary.c_str(),
                LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (exists ? 0 : LIBSSH2_FXF_EXCL), mode));
            if (!remoteFile) throw std::runtime_error("Cannot open remote temporary file");
            offset = remoteInfo(remoteFile.get()).filesize;
        } else {
            if (!remoteStat(sftp, remotePath, sourceAttr)) throw std::runtime_error("Remote source does not exist");
            remoteFile.reset(libssh2_sftp_open(sftp, remotePath.c_str(), LIBSSH2_FXF_READ, 0));
            if (!remoteFile) throw std::runtime_error("Cannot open remote source");
            sourceAttr = remoteInfo(remoteFile.get());
            state.total = sourceAttr.filesize;
            checkLocalPath(localTemporary);
            const bool exists = fs::exists(localTemporary);
            localFile.handle = CreateFileW(localTemporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                exists ? OPEN_EXISTING : CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (localFile.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open local temporary file");
            offset = sizeOf(localFile.handle);
        }
        BY_HANDLE_FILE_INFORMATION localInfo{};
        if (!GetFileInformationByHandle(localFile.handle, &localInfo) ||
            (localInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            (!upload && localInfo.nNumberOfLinks != 1))
            throw std::runtime_error("Local transfer file is a link or directory");
        if (expectedSize && state.total != *expectedSize) throw std::runtime_error("Source size changed after scanning");
        // Verify the entire existing prefix before reuse, including after crashes or a changed source.
        // A mismatch restarts only our temporary file, never the final destination.
        std::vector<char> buffer(1024 * 1024), comparison(buffer.size());
        bool matches = offset <= state.total;
        state.message = offset ? "Verifying partial file" : "Transferring";
        report(true);
        std::uint64_t checked = 0;
        while (matches && checked < offset) {
            cancelled();
            const auto count = DWORD(std::min<std::uint64_t>(buffer.size(), offset - checked));
            DWORD localRead = 0;
            if (!ReadFile(localFile.handle, buffer.data(), count, &localRead, nullptr) || localRead != count)
                throw std::runtime_error("Cannot read partial file for verification");
            std::size_t remoteRead = 0;
            while (remoteRead < count) {
                cancelled();
                const auto n = libssh2_sftp_read(remoteFile.get(), comparison.data() + remoteRead, count - remoteRead);
                if (n <= 0) throw std::runtime_error("Cannot verify remote partial file");
                remoteRead += std::size_t(n);
            }
            matches = std::equal(buffer.begin(), buffer.begin() + count, comparison.begin());
            checked += count;
        }
        if (!matches) {
            offset = 0;
            if (upload) {
                LIBSSH2_SFTP_ATTRIBUTES truncate{}; truncate.flags = LIBSSH2_SFTP_ATTR_SIZE; truncate.filesize = 0;
                if (libssh2_sftp_fstat_ex(remoteFile.get(), &truncate, 1) != 0)
                    throw std::runtime_error("Cannot restart remote temporary file");
            } else {
                position(localFile.handle, 0);
                if (!SetEndOfFile(localFile.handle)) throw std::runtime_error("Cannot restart local temporary file");
            }
        }
        position(localFile.handle, offset);
        libssh2_sftp_seek64(remoteFile.get(), offset);
        state.transferred = state.resumedBytes = offset;
        state.message = offset ? "Resuming" : "Transferring";
        started = std::chrono::steady_clock::now();
        report(true);
        while (state.transferred < state.total) {
            cancelled();
            const auto count = DWORD(std::min<std::uint64_t>(buffer.size(), state.total - state.transferred));
            if (upload) {
                DWORD read = 0;
                if (!ReadFile(localFile.handle, buffer.data(), count, &read, nullptr) || read != count)
                    throw std::runtime_error("Local source changed or read failed");
                std::size_t sent = 0;
                while (sent < read) {
                    cancelled();
                    const auto n = libssh2_sftp_write(remoteFile.get(), buffer.data() + sent, read - sent);
                    if (n <= 0) throw std::runtime_error("SFTP upload failed");
                    sent += std::size_t(n); state.transferred += std::uint64_t(n); report();
                }
            } else {
                const auto n = libssh2_sftp_read(remoteFile.get(), buffer.data(), count);
                if (n <= 0) throw std::runtime_error("SFTP download failed or source size changed");
                DWORD written = 0;
                if (!WriteFile(localFile.handle, buffer.data(), DWORD(n), &written, nullptr) || written != DWORD(n))
                    throw std::runtime_error("Cannot write local temporary file");
                state.transferred += std::uint64_t(n); report();
            }
        }
        report(true);
        cancelled();
        const auto finalAttr = remoteInfo(remoteFile.get());
        if (finalAttr.filesize != state.total ||
            (!upload && (sourceAttr.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) &&
             (!(finalAttr.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) || finalAttr.mtime != sourceAttr.mtime)))
            throw std::runtime_error("Remote source or temporary file changed during transfer");
        if (upload) {
            if (sizeOf(localFile.handle) != state.total) throw std::runtime_error("Local source size changed");
            if (libssh2_sftp_fsync(remoteFile.get()) != 0 && libssh2_sftp_last_error(sftp) != LIBSSH2_FX_OP_UNSUPPORTED)
                throw std::runtime_error("Cannot flush remote temporary file");
        } else if (!FlushFileBuffers(localFile.handle)) throw std::runtime_error("Cannot flush local temporary file");
        if (libssh2_sftp_close(remoteFile.release()) != 0) throw std::runtime_error("Cannot close remote file");
        localFile.close();
        cancelled();
        if (upload) {
            remoteStat(sftp, remotePath, attr);
            // Never unlink the old target. Unsupported atomic replacement leaves the .part for retry.
            if (libssh2_sftp_posix_rename(sftp, remoteTemporary.c_str(), remotePath.c_str()) != 0)
                throw std::runtime_error("Atomic remote replacement failed; temporary file retained");
        } else {
            checkLocalPath(local);
            if (!MoveFileExW(localTemporary.c_str(), local.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot replace local target; temporary file retained");
        }
        state.active = false; state.completed = true; state.message = "Completed";
    } catch (const std::exception& exception) {
        error = stop_.stop_requested() ? "Transfer cancelled" : exception.what();
        state.active = false; state.failed = true; state.message = error;
    }
    report(true);
    return state.completed;
}
