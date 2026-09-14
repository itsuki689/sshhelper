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

// Linker wrappers inject server failures while exercising the real transfer/file code.
namespace {
enum class Fault { None, Init, Open, Read, Write, ZeroWrite, Close, Size, Shutdown };
Fault fault = Fault::None;
std::string uploaded;
const std::string payload(100003, 'x');
std::size_t offset = 0;
int opaqueSftp, opaqueHandle;
}
extern "C" {
int __wrap_libssh2_session_handshake(LIBSSH2_SESSION*, libssh2_socket_t) { return 0; }
const char* __wrap_libssh2_hostkey_hash(LIBSSH2_SESSION*, int) {
    static const char hash[32] = "transfer-test-only";
    return hash;
}
int __wrap_libssh2_userauth_password_ex(LIBSSH2_SESSION*, const char*, unsigned int,
    const char*, unsigned int, LIBSSH2_PASSWD_CHANGEREQ_FUNC(*)) { return 0; }
LIBSSH2_SFTP* __wrap_libssh2_sftp_init(LIBSSH2_SESSION*) {
    return fault == Fault::Init ? nullptr : reinterpret_cast<LIBSSH2_SFTP*>(&opaqueSftp);
}
int __wrap_libssh2_sftp_shutdown(LIBSSH2_SFTP*) { return fault == Fault::Shutdown ? -1 : 0; }
LIBSSH2_SFTP_HANDLE* __wrap_libssh2_sftp_open_ex(LIBSSH2_SFTP*, const char*, unsigned int,
    unsigned long, long, int) {
    offset = 0;
    uploaded.clear();
    return fault == Fault::Open ? nullptr : reinterpret_cast<LIBSSH2_SFTP_HANDLE*>(&opaqueHandle);
}
int __wrap_libssh2_sftp_fstat_ex(LIBSSH2_SFTP_HANDLE*, LIBSSH2_SFTP_ATTRIBUTES* attrs, int) {
    attrs->flags = LIBSSH2_SFTP_ATTR_SIZE;
    attrs->filesize = payload.size() + (fault == Fault::Size ? 1 : 0);
    return 0;
}
ssize_t __wrap_libssh2_sftp_read(LIBSSH2_SFTP_HANDLE*, char* buffer, size_t capacity) {
    if (fault == Fault::Read) return -1;
    const auto count = std::min(capacity, payload.size() - offset);
    std::memcpy(buffer, payload.data() + offset, count);
    offset += count;
    return static_cast<ssize_t>(count);
}
ssize_t __wrap_libssh2_sftp_write(LIBSSH2_SFTP_HANDLE*, const char* buffer, size_t count) {
    if (fault == Fault::Write) return -1;
    if (fault == Fault::ZeroWrite) return 0;
    count = std::min(count, std::size_t{997}); // Force short writes.
    uploaded.append(buffer, count);
    return static_cast<ssize_t>(count);
}
int __wrap_libssh2_sftp_close_handle(LIBSSH2_SFTP_HANDLE*) { return fault == Fault::Close ? -1 : 0; }
}

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    return __LINE__; } } while (false)

int runTests() {
    // Only a TCP accept queue is needed: SSH authentication and SFTP are wrapped.
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(listen(listener, 1) == 0);
    int addressSize = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize);
    std::stop_source stop;
    SshSession session(stop.get_token(), 1000);
    ConnectionConfig config;
    config.host = "127.0.0.1";
    config.port = ntohs(address.sin_port);
    config.username = "test";
    config.password = "test";
    std::string error;
    CHECK(session.connect(config, error));
    closesocket(listener);

    const auto base = std::filesystem::temp_directory_path() /
        ("airsftp-transfer-test-" + std::to_string(GetCurrentProcessId()));
    const auto source = std::filesystem::path(base.string() + "-source.bin");
    const auto destination = std::filesystem::path(base.string() + "-destination.bin");
    struct Cleanup {
        std::filesystem::path source, destination;
        ~Cleanup() { std::error_code error; std::filesystem::remove(source, error); std::filesystem::remove(destination, error); }
    } cleanup{source, destination};
    { std::ofstream file(source, std::ios::binary); file << payload; CHECK(file.good()); }
    auto utf8 = [](const std::filesystem::path& path) {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    };
    TransferProgress final;
    auto run = [&](TransferDirection direction, Fault injected) {
        fault = injected;
        final = {};
        return session.transfer(direction, utf8(direction == TransferDirection::Upload ? source : destination),
            "/test", [&](const TransferProgress& state) { final = state; }, error);
    };
    CHECK(run(TransferDirection::Upload, Fault::None));
    CHECK(final.completed && !final.failed && final.transferred == payload.size());
    CHECK(uploaded == payload);
    CHECK(run(TransferDirection::Download, Fault::None));
    CHECK(final.completed && !final.failed && final.transferred == payload.size());
    {
        std::ifstream file(destination, std::ios::binary);
        CHECK(std::string(std::istreambuf_iterator<char>(file), {}) == payload);
    }
    for (const auto direction : {TransferDirection::Upload, TransferDirection::Download}) {
        for (const auto injected : {Fault::Init, Fault::Open, Fault::Close, Fault::Shutdown}) {
            CHECK(!run(direction, injected));
            CHECK(final.failed && !final.completed && !final.active && !error.empty());
        }
    }
    for (const auto injected : {Fault::Write, Fault::ZeroWrite}) {
        CHECK(!run(TransferDirection::Upload, injected));
        CHECK(final.failed && !final.completed);
    }
    for (const auto injected : {Fault::Read, Fault::Size}) {
        CHECK(!run(TransferDirection::Download, injected));
        CHECK(final.failed && !final.completed);
    }
    fault = Fault::None;
    CHECK(!session.transfer(TransferDirection::Upload, utf8(source) + ".missing", "/test",
        [&](const TransferProgress& state) { final = state; }, error));
    CHECK(final.failed && !final.completed);
    CHECK(!session.transfer(TransferDirection::Download, utf8(destination / "missing/file"), "/test",
        [&](const TransferProgress& state) { final = state; }, error));
    CHECK(final.failed && !final.completed);
    CHECK(!session.transfer(TransferDirection::Upload, utf8(source), "/test",
        [&](const TransferProgress& state) { final = state; if (state.active) stop.request_stop(); }, error));
    CHECK(final.failed && !final.completed && error == "Transfer cancelled");
    std::cout << "Transfer success, short writes, cancellation and injected failures: OK\n";
    return 0;
}
int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    if (libssh2_init(0) != 0) { WSACleanup(); return 1; }
    const int result = runTests();
    libssh2_exit();
    WSACleanup();
    return result;
}
