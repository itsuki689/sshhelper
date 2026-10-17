#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace restart_process {
std::filesystem::path executablePath();
// Starts a replacement that waits on an inherited SYNCHRONIZE-only parent handle.
bool launchAfterExit(const std::filesystem::path& executable, std::wstring& error);
// Used before any windows, shortcuts or monitoring sessions are created.
bool waitForParent(std::wstring_view inheritedHandle);
}
