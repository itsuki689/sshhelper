#pragma once
#include "ssh_client.hpp"
#include <winsock2.h>
#include <windows.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <limits>
namespace {
struct Node { unsigned long mode; std::string data; unsigned long mtime = 100; };
std::map<std::string, Node> remoteFiles{{"/", {LIBSSH2_SFTP_S_IFDIR, ""}}};
struct MockFile { std::string path; std::size_t offset = 0; std::vector<std::string> children; };
unsigned long lastError = 0;
bool failWrite = false, failClose = false, failMkdir = false, failRead = false, failRename = false,
     failInit = false, failOpen = false, failShutdown = false, failFlush = false, zeroWrite = false;
std::size_t failWriteAfter = SIZE_MAX, failReadAfter = SIZE_MAX, bytesWritten = 0, bytesRead = 0;
int sftpToken, initCount = 0;
std::string badName;
void attrFor(const Node& node, LIBSSH2_SFTP_ATTRIBUTES* attr) {
    *attr = {};
    attr->flags = LIBSSH2_SFTP_ATTR_PERMISSIONS | LIBSSH2_SFTP_ATTR_SIZE | LIBSSH2_SFTP_ATTR_ACMODTIME;
    attr->permissions = node.mode; attr->filesize = node.data.size(); attr->mtime = node.mtime;
}
}
extern "C" {
int __wrap_libssh2_session_handshake(LIBSSH2_SESSION*, libssh2_socket_t) { return 0; }
const char* __wrap_libssh2_hostkey_hash(LIBSSH2_SESSION*, int) {
    static const char key[32] = "folder-test-only"; return key;
}
int __wrap_libssh2_userauth_password_ex(LIBSSH2_SESSION*, const char*, unsigned int,
    const char*, unsigned int, LIBSSH2_PASSWD_CHANGEREQ_FUNC(*)) { return 0; }
LIBSSH2_SFTP* __wrap_libssh2_sftp_init(LIBSSH2_SESSION*) {
    ++initCount; return failInit ? nullptr : reinterpret_cast<LIBSSH2_SFTP*>(&sftpToken);
}
int __wrap_libssh2_sftp_shutdown(LIBSSH2_SFTP*) { return failShutdown ? -1 : 0; }
unsigned long __wrap_libssh2_sftp_last_error(LIBSSH2_SFTP*) { return lastError; }
int __wrap_libssh2_sftp_stat_ex(LIBSSH2_SFTP*, const char* path, unsigned int length, int, LIBSSH2_SFTP_ATTRIBUTES* attr) {
    const auto it = remoteFiles.find(std::string(path, length));
    if (it == remoteFiles.end()) { lastError = LIBSSH2_FX_NO_SUCH_FILE; return -1; }
    attrFor(it->second, attr); return 0;
}
int __wrap_libssh2_sftp_mkdir_ex(LIBSSH2_SFTP*, const char* path, unsigned int length, long) {
    if (failMkdir) { lastError = LIBSSH2_FX_PERMISSION_DENIED; return -1; }
    auto [it, inserted] = remoteFiles.emplace(std::string(path, length), Node{LIBSSH2_SFTP_S_IFDIR, ""});
    return inserted ? 0 : -1;
}
LIBSSH2_SFTP_HANDLE* __wrap_libssh2_sftp_open_ex(LIBSSH2_SFTP*, const char* path, unsigned int length,
    unsigned long flags, long, int type) {
    if (failOpen) return nullptr;
    std::string name(path, length);
    if (type == LIBSSH2_SFTP_OPENDIR) {
        if (!remoteFiles.contains(name) || !LIBSSH2_SFTP_S_ISDIR(remoteFiles.at(name).mode)) return nullptr;
        auto* file = new MockFile{name};
        const auto prefix = name == "/" ? name : name + "/";
        for (const auto& [key, node] : remoteFiles) {
            if (key.starts_with(prefix) && key.size() > prefix.size()) {
                const auto child = key.substr(prefix.size());
                if (child.find('/') == std::string::npos) file->children.push_back(child);
            }
        }
        if (!badName.empty()) file->children.push_back(badName);
        return reinterpret_cast<LIBSSH2_SFTP_HANDLE*>(file);
    }
    if ((flags & LIBSSH2_FXF_EXCL) && remoteFiles.contains(name)) return nullptr;
    if ((flags & LIBSSH2_FXF_CREAT) && !remoteFiles.contains(name))
        remoteFiles[name] = {LIBSSH2_SFTP_S_IFREG, ""};
    if (!remoteFiles.contains(name)) return nullptr;
    if (flags & LIBSSH2_FXF_TRUNC) remoteFiles.at(name).data.clear();
    return reinterpret_cast<LIBSSH2_SFTP_HANDLE*>(new MockFile{name});
}
int __wrap_libssh2_sftp_readdir_ex(LIBSSH2_SFTP_HANDLE* handle, char* buffer, size_t capacity,
    char*, size_t, LIBSSH2_SFTP_ATTRIBUTES* attr) {
    auto& file = *reinterpret_cast<MockFile*>(handle);
    if (file.offset == file.children.size()) return 0;
    const auto& name = file.children[file.offset++];
    if (name.size() > capacity) return -1;
    std::memcpy(buffer, name.data(), name.size());
    if (name == badName) attrFor({LIBSSH2_SFTP_S_IFREG, "bad"}, attr);
    else attrFor(remoteFiles.at(file.path + (file.path == "/" ? "" : "/") + name), attr);
    return static_cast<int>(name.size());
}
int __wrap_libssh2_sftp_fstat_ex(LIBSSH2_SFTP_HANDLE* handle, LIBSSH2_SFTP_ATTRIBUTES* attr, int set) {
    auto& node = remoteFiles.at(reinterpret_cast<MockFile*>(handle)->path);
    if (set) { if (attr->flags & LIBSSH2_SFTP_ATTR_SIZE) node.data.resize(attr->filesize); }
    else attrFor(node, attr);
    return 0;
}
void __wrap_libssh2_sftp_seek64(LIBSSH2_SFTP_HANDLE* handle, libssh2_uint64_t offset) {
    reinterpret_cast<MockFile*>(handle)->offset = std::size_t(offset);
}
ssize_t __wrap_libssh2_sftp_read(LIBSSH2_SFTP_HANDLE* handle, char* buffer, size_t capacity) {
    if (failRead || bytesRead >= failReadAfter) return -1;
    auto& file = *reinterpret_cast<MockFile*>(handle);
    const auto& data = remoteFiles.at(file.path).data;
    const auto count = std::min({capacity, data.size() - std::min(data.size(), file.offset), failReadAfter - bytesRead});
    std::memcpy(buffer, data.data() + std::min(data.size(), file.offset), count);
    file.offset += count; bytesRead += count; return count;
}
ssize_t __wrap_libssh2_sftp_write(LIBSSH2_SFTP_HANDLE* handle, const char* buffer, size_t count) {
    if (failWrite || bytesWritten >= failWriteAfter) return -1;
    if (zeroWrite) return 0;
    count = std::min({count, std::size_t{8191}, failWriteAfter - bytesWritten});
    auto& file = *reinterpret_cast<MockFile*>(handle);
    auto& data = remoteFiles.at(file.path).data;
    if (data.size() < file.offset + count) data.resize(file.offset + count);
    std::memcpy(data.data() + file.offset, buffer, count);
    file.offset += count; bytesWritten += count;
    return count;
}
int __wrap_libssh2_sftp_fsync(LIBSSH2_SFTP_HANDLE*) { return failFlush ? -1 : 0; }
int __wrap_libssh2_sftp_posix_rename_ex(LIBSSH2_SFTP*, const char* from, size_t fromSize, const char* to, size_t toSize) {
    if (failRename) return -1;
    std::string source(from, fromSize), destination(to, toSize);
    if (!remoteFiles.contains(source)) return -1;
    remoteFiles[destination] = remoteFiles.at(source); remoteFiles.erase(source); return 0;
}
int __wrap_libssh2_sftp_close_handle(LIBSSH2_SFTP_HANDLE* handle) {
    delete reinterpret_cast<MockFile*>(handle); return failClose ? -1 : 0;
}
}
