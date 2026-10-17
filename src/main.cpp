#include "app.hpp"
#include "restart_process.hpp"

#include <libssh2.h>
#include <winsock2.h>
#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && std::wstring_view(argv[1]) == L"--restart-wait-handle") {
        const bool ready = argc == 3 && restart_process::waitForParent(argv[2]);
        LocalFree(argv);
        if (!ready) return 1;
    } else if (argv) LocalFree(argv);
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
        App app(instance);
        result = app.run(showCommand);
    }

    libssh2_exit();
    WSACleanup();
    CoUninitialize();
    return result;
}
