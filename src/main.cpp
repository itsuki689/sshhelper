#include "app.hpp"
#include "restart_process.hpp"
#include "app_paths.hpp"

#include <libssh2.h>
#include <winsock2.h>
#include <windows.h>
#include <optional>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::filesystem::path requestedData, legacyDirectory;
    std::optional<std::wstring> parentHandle;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring_view option = argv[i];
        if (i + 1 >= argc) { LocalFree(argv); return 1; }
        if (option == L"--data-dir") requestedData = argv[++i];
        else if (option == L"--migrate-legacy-data") legacyDirectory = argv[++i];
        else if (option == L"--restart-wait-handle") parentHandle = argv[++i];
        else { LocalFree(argv); return 1; }
    }
    if (argv) LocalFree(argv);
    if (parentHandle && !restart_process::waitForParent(*parentHandle)) return 1;
    std::filesystem::path credentials;
    try {
        const auto directory = dataDirectory(requestedData);
        if (!legacyDirectory.empty()) {
            std::string error;
            if (!migrateLegacyData(legacyDirectory, directory, error)) {
                MessageBoxA(nullptr, error.c_str(), "sshhelper migration failed", MB_OK | MB_ICONERROR);
                return 1;
            }
        }
        credentials = directory / L"sshhelper.credentials";
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "sshhelper data directory", MB_OK | MB_ICONERROR);
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) return 1;
    if (libssh2_init(0) != 0) {
        WSACleanup();
        return 1;
    }

    int result = 0;
    {
        App app(instance, credentials);
        result = app.run(showCommand);
    }

    libssh2_exit();
    WSACleanup();
    CoUninitialize();
    return result;
}
