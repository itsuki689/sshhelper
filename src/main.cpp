#include "app.hpp"

#include <libssh2.h>
#include <winsock2.h>
#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
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
