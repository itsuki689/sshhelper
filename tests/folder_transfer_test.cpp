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

namespace {
struct Node { unsigned long mode; std::string data; };
std::map<std::string, Node> remoteFiles{{"/", {LIBSSH2_SFTP_S_IFDIR, ""}}};
struct MockFile { std::string path; std::size_t offset = 0; std::vector<std::string> children; };
unsigned long lastError = 0;
bool failWrite = false, failClose = false, failMkdir = false;
int sftpToken;
std::string badName;
void attrFor(const Node& node, LIBSSH2_SFTP_ATTRIBUTES* attr) {
    *attr = {};
    attr->flags = LIBSSH2_SFTP_ATTR_PERMISSIONS | LIBSSH2_SFTP_ATTR_SIZE;
    attr->permissions = node.mode;
    attr->filesize = node.data.size();
}
}
extern "C" {
int __wrap_libssh2_session_handshake(LIBSSH2_SESSION*, libssh2_socket_t) { return 0; }
const char* __wrap_libssh2_hostkey_hash(LIBSSH2_SESSION*, int) {
    static const char key[32] = "folder-test-only"; return key;
}
int __wrap_libssh2_userauth_password_ex(LIBSSH2_SESSION*, const char*, unsigned int,
    const char*, unsigned int, LIBSSH2_PASSWD_CHANGEREQ_FUNC(*)) { return 0; }
LIBSSH2_SFTP* __wrap_libssh2_sftp_init(LIBSSH2_SESSION*) { return reinterpret_cast<LIBSSH2_SFTP*>(&sftpToken); }
int __wrap_libssh2_sftp_shutdown(LIBSSH2_SFTP*) { return 0; }
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
    if (flags & LIBSSH2_FXF_WRITE) remoteFiles[name] = {LIBSSH2_SFTP_S_IFREG, ""};
    if (!remoteFiles.contains(name)) return nullptr;
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
int __wrap_libssh2_sftp_fstat_ex(LIBSSH2_SFTP_HANDLE* handle, LIBSSH2_SFTP_ATTRIBUTES* attr, int) {
    attrFor(remoteFiles.at(reinterpret_cast<MockFile*>(handle)->path), attr); return 0;
}
ssize_t __wrap_libssh2_sftp_read(LIBSSH2_SFTP_HANDLE* handle, char* buffer, size_t capacity) {
    auto& file = *reinterpret_cast<MockFile*>(handle);
    const auto& data = remoteFiles.at(file.path).data;
    const auto count = std::min(capacity, data.size() - file.offset);
    std::memcpy(buffer, data.data() + file.offset, count);
    file.offset += count; return count;
}
ssize_t __wrap_libssh2_sftp_write(LIBSSH2_SFTP_HANDLE* handle, const char* buffer, size_t count) {
    if (failWrite) return -1;
    count = std::min(count, std::size_t{8191});
    remoteFiles.at(reinterpret_cast<MockFile*>(handle)->path).data.append(buffer, count);
    return count;
}
int __wrap_libssh2_sftp_close_handle(LIBSSH2_SFTP_HANDLE* handle) {
    delete reinterpret_cast<MockFile*>(handle); return failClose ? -1 : 0;
}
}
#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c << "\n" << error << "\n"; return __LINE__; } } while(false)

int runTests() {
    namespace fs = std::filesystem;
    std::string error;
    const auto root = fs::temp_directory_path() / ("airsftp-folder-" + std::to_string(GetCurrentProcessId()));
    CHECK(!fs::exists(root));
    fs::create_directory(root);
    struct Cleanup { fs::path root; ~Cleanup() { std::error_code ec; fs::remove_all(root, ec); } } cleanup{root};
    const auto source = root / "source";
    fs::create_directories(source / "sub/empty");
    fs::create_directories(root / "empty");
    { std::ofstream(source / "a.txt") << "alpha"; }
    { std::ofstream(source / "sub/b.txt", std::ios::binary) << std::string(70001, 'b'); }
    { std::ofstream(source / fs::path(L"\u4e2d\u6587.txt"), std::ios::binary) << "unicode"; }
    { std::ofstream(source / "zero"); }
    auto utf8 = [](const fs::path& path) { const auto s = path.generic_u8string(); return std::string(s.begin(), s.end()); };
    auto read = [](const fs::path& path) { std::ifstream f(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    std::stop_source stop;
    SshSession session(stop.get_token());
    std::vector<TransferEntry> entries;
    CHECK(session.listFolder(TransferDirection::Upload, utf8(source), entries, error));
    CHECK(entries.size() == 6);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(listen(listener, 1) == 0);
    int size = sizeof(addr); getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &size);
    ConnectionConfig config; config.host = "127.0.0.1"; config.port = ntohs(addr.sin_port); config.username = "test";
    CHECK(session.connect(config, error));
    closesocket(listener);
    TransferProgress final;
    auto progress = [&](const TransferProgress& value) { final = value; };
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(source), "/out/new", std::nullopt, progress, error));
    CHECK(final.completed && final.filesCompleted == 4 && final.filesTotal == 4 && final.total == 70013);
    CHECK(final.transferred == final.total);
    CHECK(remoteFiles.at("/out/new/a.txt").data == "alpha");
    CHECK(remoteFiles.at("/out/new/sub/b.txt").data == std::string(70001, 'b'));
    CHECK(LIBSSH2_SFTP_S_ISDIR(remoteFiles.at("/out/new/sub/empty").mode));
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(source), "/partial",
        std::vector<std::string>{"sub/b.txt"}, progress, error));
    CHECK(remoteFiles.contains("/partial/sub/b.txt") && !remoteFiles.contains("/partial/a.txt"));
    CHECK(!remoteFiles.contains("/partial/sub/empty"));
    CHECK(final.filesCompleted == 1);

    CHECK(session.transferFolder(TransferDirection::Download, utf8(root / "download"), "/out/new",
        std::nullopt, progress, error));
    CHECK(read(root / "download/a.txt") == "alpha");
    CHECK(read(root / "download/sub/b.txt") == std::string(70001, 'b'));
    CHECK(read(root / "download" / fs::path(L"\u4e2d\u6587.txt")) == "unicode");
    CHECK(fs::is_directory(root / "download/sub/empty") && fs::file_size(root / "download/zero") == 0);
    CHECK(final.filesCompleted == 4 && final.transferred == final.total);
    CHECK(session.transferFolder(TransferDirection::Download, utf8(root / "selected"), "/out/new",
        std::vector<std::string>{"sub/b.txt", "sub/empty"}, progress, error));
    CHECK(!fs::exists(root / "selected/a.txt"));
    CHECK(fs::is_directory(root / "selected/sub/empty"));
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(root / "empty"), "/empty", std::nullopt, progress, error));
    CHECK(final.completed && final.filesTotal == 0 && remoteFiles.contains("/empty"));
    CHECK(session.transferFolder(TransferDirection::Download, utf8(root / "empty-download"), "/empty", std::nullopt, progress, error));
    CHECK(fs::is_directory(root / "empty-download"));
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/nothing",
        std::vector<std::string>{}, progress, error));
    CHECK(!remoteFiles.contains("/nothing"));
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/nothing",
        std::vector<std::string>{"../a.txt"}, progress, error));
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/../escape", std::nullopt, progress, error));
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(source), "/", std::vector<std::string>{"a.txt"}, progress, error));
    CHECK(remoteFiles.at("/a.txt").data == "alpha");
    remoteFiles["/case"] = {LIBSSH2_SFTP_S_IFDIR, ""};
    remoteFiles["/case/A.txt"] = {LIBSSH2_SFTP_S_IFREG, "A"};
    remoteFiles["/case/a.txt"] = {LIBSSH2_SFTP_S_IFREG, "a"};
    CHECK(!session.transferFolder(TransferDirection::Download, utf8(root / "case"), "/case", std::nullopt, progress, error));
    CHECK(!fs::exists(root / "case"));
    remoteFiles["/mixed"] = {LIBSSH2_SFTP_S_IFDIR, ""};
    remoteFiles["/mixed/okay.txt"] = {LIBSSH2_SFTP_S_IFREG, "okay"};
    remoteFiles["/mixed/bad:name"] = {LIBSSH2_SFTP_S_IFREG, "not selectable on Windows"};
    CHECK(session.transferFolder(TransferDirection::Download, utf8(root / "mixed"), "/mixed",
        std::vector<std::string>{"okay.txt"}, progress, error));
    CHECK(read(root / "mixed/okay.txt") == "okay");
    remoteFiles["/parents"] = {LIBSSH2_SFTP_S_IFDIR, ""};
    remoteFiles["/parents/Foo"] = {LIBSSH2_SFTP_S_IFDIR, ""};
    remoteFiles["/parents/foo"] = {LIBSSH2_SFTP_S_IFDIR, ""};
    remoteFiles["/parents/Foo/a"] = {LIBSSH2_SFTP_S_IFREG, "A"};
    remoteFiles["/parents/foo/b"] = {LIBSSH2_SFTP_S_IFREG, "B"};
    CHECK(!session.transferFolder(TransferDirection::Download, utf8(root / "parents"), "/parents",
        std::vector<std::string>{"Foo/a", "foo/b"}, progress, error));
    CHECK(!fs::exists(root / "parents"));
    remoteFiles["/bad"] = {LIBSSH2_SFTP_S_IFDIR, ""};
    remoteFiles["/bad/CON.txt"] = {LIBSSH2_SFTP_S_IFREG, "bad"};
    CHECK(!session.transferFolder(TransferDirection::Download, utf8(root / "bad"), "/bad", std::nullopt, progress, error));
    CHECK(!fs::exists(root / "bad"));
    badName = "../escape";
    CHECK(!session.listFolder(TransferDirection::Download, "/empty", entries, error));
    badName.clear();
    remoteFiles["/empty/link"] = {LIBSSH2_SFTP_S_IFLNK, "/out"};
    CHECK(session.listFolder(TransferDirection::Download, "/empty", entries, error) && entries.empty());
    CHECK(session.uploadPaths({utf8(source / "a.txt"), utf8(source / "sub"),
        utf8(source / fs::path(L"中文.txt")), utf8(source / "zero")}, "/dropped", progress, error));
    CHECK(final.completed && final.filesCompleted == 4 && final.total == 70013 && final.transferred == final.total);
    CHECK(remoteFiles.at("/dropped/a.txt").data == "alpha");
    CHECK(remoteFiles.at("/dropped/sub/b.txt").data == std::string(70001, 'b'));
    CHECK(LIBSSH2_SFTP_S_ISDIR(remoteFiles.at("/dropped/sub/empty").mode));
    CHECK(remoteFiles.at("/dropped/中文.txt").data == "unicode");
    CHECK(!remoteFiles.contains("/dropped/source"));
    CHECK(session.uploadPaths({utf8(source / "a.txt"), utf8(source / "a.txt")}, "/deduplicated", progress, error));
    CHECK(final.filesCompleted == 1);
    fs::create_directory(root / "other");
    { std::ofstream(root / "other/a.txt") << "different"; }
    CHECK(!session.uploadPaths({utf8(source / "a.txt"), utf8(root / "other/a.txt")}, "/collision", progress, error));
    CHECK(final.failed && !remoteFiles.contains("/collision"));
    CHECK(!session.uploadPaths({}, "/no-drop", progress, error));
    CHECK(!session.uploadPaths({"relative.txt"}, "/no-drop", progress, error));
    CHECK(!session.uploadPaths({utf8(root / "missing")}, "/no-drop", progress, error));
    CHECK(!remoteFiles.contains("/no-drop"));
    CHECK(session.uploadPaths({utf8(root / "empty")}, "/empty-drop", progress, error));
    CHECK(final.completed && final.filesTotal == 0 && remoteFiles.contains("/empty-drop/empty"));

    failWrite = true;
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/failed", std::nullopt, progress, error));
    CHECK(final.failed && !final.completed);
    CHECK(!session.uploadPaths({utf8(source / "a.txt")}, "/drop-failure", progress, error));
    CHECK(final.failed && !final.completed);
    failWrite = false; failClose = true;
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/close", std::nullopt, progress, error));
    CHECK(final.failed && !final.completed);
    failClose = false; failMkdir = true;
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/denied", std::nullopt, progress, error));
    CHECK(final.failed && !final.completed);
    failMkdir = false;
    stop.request_stop();
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(source), "/cancelled", std::nullopt, progress, error));
    CHECK(final.failed && !final.completed && error == "Transfer cancelled");
    CHECK(!session.uploadPaths({utf8(source / "a.txt")}, "/drop-cancelled", progress, error));
    CHECK(final.failed && error == "Transfer cancelled");
    std::cout << "Recursive/selected folder upload and download, empty/Unicode files, failures and path validation: OK\n";
    return 0;
}
int main() {
    WSADATA data{}; if (WSAStartup(MAKEWORD(2,2), &data) != 0) return 1;
    if (libssh2_init(0) != 0) { WSACleanup(); return 1; }
    const int result = runTests();
    libssh2_exit(); WSACleanup(); return result;
}
