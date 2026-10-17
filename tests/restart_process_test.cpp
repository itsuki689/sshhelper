#include "restart_process.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n"; return __LINE__; } } while (false)

fs::path marker() {
    wchar_t path[32768]{};
    const auto count = GetEnvironmentVariableW(L"AIRSFTP_RESTART_TEST_PATH", path, 32768);
    return count && count < 32768 ? fs::path(path) : fs::path{};
}
fs::path part(const fs::path& path, const wchar_t* suffix) { return fs::path(path.wstring() + suffix); }
bool waitForFile(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists(path) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return fs::exists(path);
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--restart-wait-handle") {
        const auto path = marker(); CHECK(!path.empty());
        std::ofstream(part(path, L".child-started")) << "waiting";
        const std::string number = argv[2];
        CHECK(restart_process::waitForParent(std::wstring(number.begin(), number.end())));
        CHECK(fs::exists(part(path, L".parent-finished")));
        std::ofstream(part(path, L".done")) << "restarted";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--test-parent") {
        const auto path = marker(); CHECK(!path.empty());
        std::wstring error;
        CHECK(restart_process::launchAfterExit(restart_process::executablePath(), error));
        CHECK(waitForFile(part(path, L".release")));
        std::ofstream(part(path, L".parent-finished")) << "exiting";
        return 0;
    }
    CHECK(!restart_process::waitForParent(L""));
    CHECK(!restart_process::waitForParent(L"0"));
    CHECK(!restart_process::waitForParent(L"invalid"));
    CHECK(!restart_process::waitForParent(L"184467440737095516160"));
    const auto executable = restart_process::executablePath(); CHECK(fs::exists(executable));
    std::wstring error;
    CHECK(!restart_process::launchAfterExit(executable.parent_path() / L"nonexistent-restart-target.exe", error));
    CHECK(!error.empty());
    const auto path = fs::temp_directory_path() / (L"AirSFTP-restart-test-" + std::to_wstring(GetCurrentProcessId()));
    CHECK(SetEnvironmentVariableW(L"AIRSFTP_RESTART_TEST_PATH", path.c_str()));
    std::wstring command = L"\"" + executable.wstring() + L"\" --test-parent";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION parent{};
    CHECK(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        executable.parent_path().c_str(), &startup, &parent));
    CHECK(waitForFile(part(path, L".child-started")));
    CHECK(WaitForSingleObject(parent.hProcess, 0) == WAIT_TIMEOUT);
    CHECK(!fs::exists(part(path, L".done")));
    std::ofstream(part(path, L".release")) << "exit now";
    CHECK(WaitForSingleObject(parent.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD code = 1; CHECK(GetExitCodeProcess(parent.hProcess, &code) && code == 0);
    CloseHandle(parent.hThread); CloseHandle(parent.hProcess);
    CHECK(waitForFile(part(path, L".done")));
    for (const auto* suffix : {L".child-started", L".release", L".parent-finished", L".done"}) fs::remove(part(path, suffix));
    SetEnvironmentVariableW(L"AIRSFTP_RESTART_TEST_PATH", nullptr);
    std::cout << "Replacement waited for its parent; launch failure preserved the caller.\n";
    return 0;
}
