#include "ssh_client.hpp"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Sftp = std::unique_ptr<LIBSSH2_SFTP, decltype(&libssh2_sftp_shutdown)>;
using Handle = std::unique_ptr<LIBSSH2_SFTP_HANDLE, decltype(&libssh2_sftp_close_handle)>;

std::string encoded(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
bool validRelative(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." ||
            part.find('\0') != std::string::npos)
            return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
std::string remoteRoot(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty() || path.front() != '/' || (path != "/" && !validRelative(path.substr(1))))
        throw std::runtime_error("Remote folder must be an absolute path without . or .. components");
    return path;
}
std::string remotePath(const std::string& root, const std::string& relative) {
    return root + (root == "/" ? "" : "/") + relative;
}
std::wstring windowsKey(const std::string& relative) {
    if (!validRelative(relative) || relative.find('\\') != std::string::npos)
        throw std::runtime_error("Path cannot be represented on Windows: " + relative);
    const auto path = fs::u8path(relative);
    for (const auto& component : path) {
        auto part = component.wstring();
        if (part.empty() || part.back() == L'.' || part.back() == L' ' ||
            part.find_first_of(L"<>:\"|?*") != std::wstring::npos ||
            std::any_of(part.begin(), part.end(), [](wchar_t c) { return c < 32; }))
            throw std::runtime_error("File name cannot be represented on Windows: " + relative);
        CharUpperBuffW(part.data(), static_cast<DWORD>(part.size()));
        const auto stem = part.substr(0, part.find(L'.'));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
             ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' ||
              stem[3] == L'\u00b2' || stem[3] == L'\u00b3')))
            throw std::runtime_error("Reserved Windows file name: " + relative);
    }
    auto result = path.wstring();
    CharUpperBuffW(result.data(), static_cast<DWORD>(result.size()));
    return result;
}
// Check every existing component so directory junctions cannot redirect a download.
void checkLocal(const fs::path& path, bool directory, bool create) {
    const auto full = fs::absolute(path).lexically_normal();
    auto current = full.root_path();
    for (auto it = full.begin(); it != full.end(); ++it) {
        if (*it == full.root_name() || *it == full.root_directory()) continue;
        current /= *it;
        const bool last = current == full;
        const bool needDirectory = !last || directory;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("Links/junctions are not supported: " + encoded(current));
            if (static_cast<bool>(attributes & FILE_ATTRIBUTE_DIRECTORY) != needDirectory)
                throw std::runtime_error("File/folder conflict: " + encoded(current));
        } else {
            const DWORD code = GetLastError();
            if (!create || (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND))
                throw std::runtime_error("Cannot access local path: " + encoded(current));
            if (needDirectory) fs::create_directory(current);
        }
    }
}
LIBSSH2_SFTP_ATTRIBUTES attributes(LIBSSH2_SFTP* sftp, const std::string& path) {
    LIBSSH2_SFTP_ATTRIBUTES result{};
    if (libssh2_sftp_lstat(sftp, path.c_str(), &result) != 0 ||
        !(result.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
        throw std::runtime_error("Cannot inspect remote path: " + path);
    return result;
}
void ensureRemoteDirectory(LIBSSH2_SFTP* sftp, const std::string& path, bool create,
                           std::set<std::string>* checked = nullptr) {
    if (path == "/") return;
    std::size_t end = 1;
    for (;;) {
        end = path.find('/', end);
        const auto part = path.substr(0, end);
        if (checked && checked->contains(part)) {
            if (end == std::string::npos) break;
            ++end; continue;
        }
        LIBSSH2_SFTP_ATTRIBUTES attr{};
        if (libssh2_sftp_lstat(sftp, part.c_str(), &attr) != 0) {
            if (!create || libssh2_sftp_last_error(sftp) != LIBSSH2_FX_NO_SUCH_FILE)
                throw std::runtime_error("Cannot inspect remote folder: " + part);
            if (libssh2_sftp_mkdir(sftp, part.c_str(), 0755) != 0) {
                attr = attributes(sftp, part); // Another process may have created it.
            } else {
                attr.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
                attr.permissions = LIBSSH2_SFTP_S_IFDIR;
            }
        }
        if (!(attr.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISDIR(attr.permissions))
            throw std::runtime_error("Remote folder is a link or file: " + part);
        if (checked) checked->insert(part);
        if (end == std::string::npos) break;
        ++end;
    }
}
bool isTemporary(const std::string& name) {
    return name.size() == 77 && name.starts_with(".airsftp-") && name.ends_with(".part");
}
}

bool SshSession::listFolder(TransferDirection direction, const std::string& folder,
                            std::vector<TransferEntry>& entries, std::string& error) {
    entries.clear();
    error.clear();
    try {
        auto cancelled = [&] {
            if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
        };
        if (direction == TransferDirection::Upload) {
            const auto root = fs::absolute(fs::u8path(folder)).lexically_normal();
            checkLocal(root, true, false);
            for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
                cancelled();
                const auto flags = GetFileAttributesW(it->path().c_str());
                if (flags == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Cannot inspect local entry");
                if (flags & FILE_ATTRIBUTE_REPARSE_POINT) { it.disable_recursion_pending(); continue; }
                if (isTemporary(encoded(it->path().filename()))) { it.disable_recursion_pending(); continue; }
                const auto relative = encoded(it->path().lexically_relative(root));
                if (!validRelative(relative)) throw std::runtime_error("Unsupported file name: " + relative);
                const bool directory = it->is_directory();
                if (!directory && !it->is_regular_file()) continue;
                entries.push_back({relative, directory, directory ? 0 : it->file_size()});
                if (it.depth() > 128 || entries.size() > 100000)
                    throw std::runtime_error("Folder exceeds the 100000-entry / 128-level limit");
            }
        } else {
            if (!session_) throw std::runtime_error("SSH session is not connected");
            const auto root = remoteRoot(folder);
            Sftp sftp(libssh2_sftp_init(session_), libssh2_sftp_shutdown);
            if (!sftp) throw std::runtime_error("Cannot initialize SFTP");
            ensureRemoteDirectory(sftp.get(), root, false);
            std::vector<std::string> pending{""};
            for (std::size_t index = 0; index < pending.size(); ++index) {
                cancelled();
                const auto relative = pending[index];
                const auto path = relative.empty() ? root : remotePath(root, relative);
                Handle handle(libssh2_sftp_opendir(sftp.get(), path.c_str()), libssh2_sftp_close_handle);
                if (!handle) throw std::runtime_error("Cannot list remote folder: " + path);
                for (;;) {
                    cancelled();
                    std::array<char, 32768> name{};
                    LIBSSH2_SFTP_ATTRIBUTES attr{};
                    const int count = libssh2_sftp_readdir(handle.get(), name.data(), name.size(), &attr);
                    if (count == 0) break;
                    if (count < 0) throw std::runtime_error("Cannot read remote folder: " + path);
                    const std::string leaf(name.data(), count);
                    if (leaf == "." || leaf == ".." || isTemporary(leaf)) continue;
                    if (leaf.find('/') != std::string::npos || !validRelative(leaf))
                        throw std::runtime_error("Unsupported remote file name: " + leaf);
                    const auto child = relative.empty() ? leaf : relative + "/" + leaf;
                    if (!(attr.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
                        attr = attributes(sftp.get(), remotePath(root, child));
                    const bool directory = LIBSSH2_SFTP_S_ISDIR(attr.permissions);
                    if (!directory && !LIBSSH2_SFTP_S_ISREG(attr.permissions)) continue;
                    if (!directory && !(attr.flags & LIBSSH2_SFTP_ATTR_SIZE))
                        throw std::runtime_error("Remote file size unavailable: " + child);
                    entries.push_back({child, directory, directory ? 0 : attr.filesize});
                    if (std::count(child.begin(), child.end(), '/') > 128 || entries.size() > 100000)
                        throw std::runtime_error("Folder exceeds the 100000-entry / 128-level limit");
                    if (directory) pending.push_back(child);
                }
                if (libssh2_sftp_close(handle.release()) != 0)
                    throw std::runtime_error("Cannot close remote folder: " + path);
            }
        }
        if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.relativePath < b.relativePath;
        });
        return true;
    } catch (const std::exception& exception) {
        entries.clear();
        error = exception.what();
        return false;
    }
}

bool SshSession::transferFolder(TransferDirection direction, const std::string& localFolder,
    const std::string& remoteFolder, const std::optional<std::vector<std::string>>& selection,
    const std::function<void(const TransferProgress&)>& progress, std::string& error, const TransferConfirmation& confirm) {
    return transferItems(direction, localFolder, remoteFolder, selection, nullptr, progress, error, confirm);
}

bool SshSession::uploadPaths(const std::vector<std::string>& paths, const std::string& remoteFolder,
    const std::function<void(const TransferProgress&)>& progress, std::string& error, const TransferConfirmation& confirm) {
    return transferItems(TransferDirection::Upload, ".", remoteFolder, std::nullopt, &paths, progress, error, confirm);
}

bool SshSession::transferItems(TransferDirection direction, const std::string& localFolder,
    const std::string& remoteFolder, const std::optional<std::vector<std::string>>& selection,
    const std::vector<std::string>* drops,
    const std::function<void(const TransferProgress&)>& progress, std::string& error, const TransferConfirmation& confirm) {
    TransferProgress state;
    state.active = true;
    state.direction = direction;
    state.fileName = "Scanning folder...";
    const auto started = std::chrono::steady_clock::now();
    auto lastReport = started - std::chrono::seconds(1);
    auto report = [&](bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastReport < std::chrono::milliseconds(100)) return;
        lastReport = now;
        const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        state.bytesPerSecond = seconds > 0 ? static_cast<std::uint64_t>((state.transferred - state.resumedBytes) / seconds) : 0;
        progress(state);
    };
    report();
    error.clear();
    try {
        if (!session_) throw std::runtime_error("SSH session is not connected");
        const auto remote = remoteRoot(remoteFolder);
        const auto local = fs::absolute(fs::u8path(localFolder)).lexically_normal();
        std::vector<TransferEntry> entries;
        std::map<std::string, fs::path> dropSources;
        if (drops) {
            if (drops->empty() || drops->size() > 100000) throw std::runtime_error("No files selected or too many dropped paths");
            std::set<std::wstring> roots;
            std::vector<fs::path> paths;
            for (const auto& name : *drops) {
                const auto path = fs::u8path(name).lexically_normal();
                if (!path.is_absolute() || path.filename().empty())
                    throw std::runtime_error("Drop an absolute file or folder path, not a drive root");
                const auto key = windowsKey(encoded(path.relative_path()));
                const auto fullKey = path.root_name().wstring() + key;
                if (roots.insert(fullKey).second) paths.push_back(path);
            }
            for (const auto& path : paths) {
                if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
                const bool directory = fs::is_directory(path);
                checkLocal(path, directory, false);
                if (!directory && !fs::is_regular_file(path))
                    throw std::runtime_error("Dropped path is not a regular file");
                const auto base = encoded(path.filename());
                entries.push_back({base, directory, directory ? 0 : fs::file_size(path)});
                dropSources.emplace(base, path);
                if (directory) {
                    std::vector<TransferEntry> children;
                    if (!listFolder(TransferDirection::Upload, encoded(path), children, error))
                        throw std::runtime_error(error);
                    for (auto& child : children) {
                        const auto source = path / fs::u8path(child.relativePath);
                        child.relativePath = base + "/" + child.relativePath;
                        dropSources.emplace(child.relativePath, source);
                        entries.push_back(std::move(child));
                    }
                }
                if (entries.size() > 100000) throw std::runtime_error("Drop exceeds 100000 entries");
            }
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.relativePath < b.relativePath;
            });
        } else if (!listFolder(direction, direction == TransferDirection::Upload ? localFolder : remote,
                               entries, error)) throw std::runtime_error(error);
        if (selection) {
            if (selection->empty()) throw std::runtime_error("No files or folders selected");
            std::set<std::string> wanted(selection->begin(), selection->end());
            std::erase_if(entries, [&](const auto& entry) {
                return wanted.erase(entry.relativePath) == 0;
            });
            if (!wanted.empty()) throw std::runtime_error("Selected entry no longer exists: " + *wanted.begin());
        }
        // Reject unrepresentable names and case collisions before touching the destination.
        std::set<std::wstring> names;
        std::map<std::wstring, std::string> parentNames;
        for (const auto& entry : entries) {
            if (!names.insert(windowsKey(entry.relativePath)).second)
                throw std::runtime_error("Case-insensitive file name collision: " + entry.relativePath);
            for (auto slash = entry.relativePath.find('/'); slash != std::string::npos;
                 slash = entry.relativePath.find('/', slash + 1)) {
                const auto prefix = entry.relativePath.substr(0, slash);
                const auto [parent, inserted] = parentNames.emplace(windowsKey(prefix), prefix);
                if (!inserted && parent->second != prefix)
                    throw std::runtime_error("Case-insensitive folder collision: " + prefix);
            }
            if (!entry.directory) {
                if (entry.size > std::numeric_limits<std::uint64_t>::max() - state.total)
                    throw std::runtime_error("Transfer size overflow");
                state.total += entry.size;
                ++state.filesTotal;
            }
        }
        if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
        if (state.filesTotal > 5 && (!confirm || !confirm(state.filesTotal, state.total)))
            throw std::runtime_error("Batch transfer not confirmed");
        if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
        Sftp sftp(libssh2_sftp_init(session_), libssh2_sftp_shutdown);
        if (!sftp) throw std::runtime_error("Cannot initialize SFTP");
        std::set<std::string> checkedDirectories;
        if (direction == TransferDirection::Upload) ensureRemoteDirectory(sftp.get(), remote, true, &checkedDirectories);
        else checkLocal(local, true, true);
        std::uint64_t completedBytes = 0, completedResumed = 0;
        for (const auto& entry : entries) {
            if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
            const auto localFile = drops ? dropSources.at(entry.relativePath) : local / fs::u8path(entry.relativePath);
            const auto remoteFile = remotePath(remote, entry.relativePath);
            state.fileName = entry.relativePath;
            if (direction == TransferDirection::Upload) {
                checkLocal(localFile, entry.directory, false);
                ensureRemoteDirectory(sftp.get(), entry.directory ? remoteFile :
                    remoteFile.substr(0, std::max(std::size_t{1}, remoteFile.find_last_of('/'))), true, &checkedDirectories);

            } else {
                ensureRemoteDirectory(sftp.get(), entry.directory ? remoteFile :
                    remoteFile.substr(0, std::max(std::size_t{1}, remoteFile.find_last_of('/'))), false, &checkedDirectories);

                checkLocal(localFile, entry.directory, true);
            }
            if (!entry.directory) {
                std::uint64_t fileBytes = 0;
                if (!transferOnSftp(sftp.get(), direction, encoded(localFile), remoteFile, [&](const TransferProgress& part) {
                    fileBytes = part.transferred;
                    state.transferred = completedBytes + part.transferred;
                    state.resumedBytes = completedResumed + part.resumedBytes;
                    state.message = part.message;
                    report();
                }, error, entry.size)) throw std::runtime_error(entry.relativePath + ": " + error);
                if (fileBytes != entry.size) throw std::runtime_error("Source size changed: " + entry.relativePath);
                completedBytes += fileBytes;
                completedResumed = state.resumedBytes;
                ++state.filesCompleted;
            }
            report();
        }
        if (libssh2_sftp_shutdown(sftp.release()) != 0) throw std::runtime_error("Cannot close SFTP session");
        if (stop_.stop_requested()) throw std::runtime_error("Transfer cancelled");
        state.active = false;
        state.completed = true;
        state.message = "Completed (" + std::to_string(state.filesCompleted) + " files)";
    } catch (const std::exception& exception) {
        error = stop_.stop_requested() ? "Transfer cancelled" : exception.what();
        state.active = false;
        state.failed = true;
        state.message = error;
    }
    report(true);
    return state.completed;
}
