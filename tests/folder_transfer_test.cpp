#include "mock_sftp.hpp"
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
    const int beforeUpload = initCount;
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(source), "/out/new", std::nullopt, progress, error));
    CHECK(final.completed && final.filesCompleted == 4 && final.filesTotal == 4 && final.total == 70013);
    CHECK(final.transferred == final.total);
    CHECK(initCount == beforeUpload + 1);
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

    fs::create_directory(root / "six");
    for (int i = 0; i < 6; ++i) { std::ofstream(root / "six" / (std::to_string(i) + ".txt")) << "hello"; }
    int confirmations = 0;
    auto reject = [&](std::size_t count, std::uint64_t bytes) { ++confirmations; return false; };
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(root / "six"), "/reject", std::nullopt,
        progress, error, reject));
    CHECK(confirmations == 1 && !remoteFiles.contains("/reject"));
    CHECK(!session.uploadPaths({utf8(root / "six")}, "/drop-reject", progress, error, reject));
    CHECK(confirmations == 2 && !remoteFiles.contains("/drop-reject"));
    CHECK(!session.transferFolder(TransferDirection::Upload, utf8(root / "six"), "/no-confirm",
        std::nullopt, progress, error));
    CHECK(!remoteFiles.contains("/no-confirm"));
    auto approve = [&](std::size_t count, std::uint64_t bytes) { ++confirmations; return count == 6 && bytes == 30; };
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(root / "six"), "/six", std::nullopt,
        progress, error, approve));
    CHECK(confirmations == 3 && final.filesCompleted == 6);
    CHECK(!session.transferFolder(TransferDirection::Download, utf8(root / "reject-download"), "/six",
        std::nullopt, progress, error, reject));
    CHECK(confirmations == 4 && !fs::exists(root / "reject-download"));
    CHECK(session.transferFolder(TransferDirection::Upload, utf8(root / "six"), "/five",
        std::vector<std::string>{"0.txt","1.txt","2.txt","3.txt","4.txt"}, progress, error, reject));
    CHECK(confirmations == 4 && final.filesCompleted == 5);
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
