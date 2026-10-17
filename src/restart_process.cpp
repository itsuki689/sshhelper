#include "restart_process.hpp"
#include <windows.h>
#include <limits>
#include <vector>

namespace restart_process {
std::filesystem::path executablePath() {
    std::vector<wchar_t> buffer(32768);
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

bool launchAfterExit(const std::filesystem::path& executable, std::wstring& error) {
    error.clear();
    HANDLE parent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
            SYNCHRONIZE, TRUE, 0)) {
        error = L"无法创建重启等待句柄。"; return false;
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    bool launched = false;
    DWORD failure = initialized ? ERROR_SUCCESS : GetLastError();
    if (initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &parent, sizeof(parent), nullptr, nullptr)) {
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + executable.wstring() + L"\" --restart-wait-handle " +
            std::to_wstring(reinterpret_cast<ULONG_PTR>(parent));
        launched = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process) != FALSE;
        failure = launched ? ERROR_SUCCESS : GetLastError();
        if (launched) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    } else if (initialized) failure = GetLastError();
    if (initialized) DeleteProcThreadAttributeList(attributes);
    CloseHandle(parent);
    if (!launched) error = L"无法启动重启进程（Windows 错误 " + std::to_wstring(failure) + L"）。";
    return launched;
}

bool waitForParent(std::wstring_view argument) {
    ULONG_PTR value = 0;
    if (argument.empty()) return false;
    for (const auto c : argument) {
        if (c < L'0' || c > L'9') return false;
        const auto digit = static_cast<unsigned>(c - L'0');
        if (value > (std::numeric_limits<ULONG_PTR>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (!value || value == std::numeric_limits<ULONG_PTR>::max()) return false;
    const auto handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags)) return false;
    const auto status = WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
    return status == WAIT_OBJECT_0;
}
}
