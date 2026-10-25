#include "credential_store.hpp"
#include "mcp_server.hpp"
#include <winsock2.h>
#include <windows.h>
#include <libssh2.h>
#include <fcntl.h>
#include <io.h>
#include <iostream>

namespace {
std::string utf8(const std::wstring& text) {
    const auto value = std::filesystem::path(text).u8string();
    return {value.begin(), value.end()};
}
int run(int argc, wchar_t** argv) {
    auto credentials = defaultCredentialPath();
    std::string mode = "--stdio", server;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            std::cout << "sshhelper-tool [--stdio | --list-servers | --snapshot] [--credentials FILE] [--server ID]\n"
                "Default: MCP JSON-RPC over stdin/stdout. Read-only; uses saved AirSFTP profiles and host-key pins.\n";
            return 0;
        }
        if (arg == L"--version") { std::cout << "sshhelper-tool 1.0.0\n"; return 0; }
        if (arg == L"--credentials" && i + 1 < argc) credentials = std::filesystem::absolute(argv[++i]);
        else if (arg == L"--server" && i + 1 < argc) server = utf8(argv[++i]);
        else if (arg == L"--stdio" || arg == L"--snapshot" || arg == L"--list-servers") mode = utf8(arg);
        else throw std::invalid_argument("Unknown or incomplete command line argument");
    }
    monitor_api::Service service(credentials);
    if (mode == "--stdio") {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        sshhelper_mcp::Server mcp([&](const std::string& name, const monitor_api::Json& args) {
            return name == "list_servers" ? service.listServers() : service.getMonitoring(args);
        });
        mcp.serve(std::cin, std::cout);
    } else {
        const auto result = mode == "--list-servers" ? service.listServers() :
            service.getMonitoring(server.empty() ? monitor_api::Json::object() : monitor_api::Json{{"server", server}});
        std::cout << monitor_api::serialize(result) << '\n';
    }
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    if (libssh2_init(0) != 0) { WSACleanup(); return 1; }
    int result;
    try { result = run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    libssh2_exit(); WSACleanup();
    return result;
}
