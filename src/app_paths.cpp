#include "app_paths.hpp"
#include "credential_store.hpp"
#include <windows.h>
#include <shlobj.h>
#include <stdexcept>
#include <vector>

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) throw std::runtime_error("Cannot locate sshhelper executable");
    return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
}
std::filesystem::path dataDirectory(const std::filesystem::path& requested) {
    namespace fs = std::filesystem;
    if (!requested.empty()) return fs::absolute(requested).lexically_normal();
    const auto length = GetEnvironmentVariableW(L"SSHHELPER_DATA_DIR", nullptr, 0);
    if (length) {
        std::vector<wchar_t> value(length);
        const auto copied = GetEnvironmentVariableW(L"SSHHELPER_DATA_DIR", value.data(), length);
        if (!copied || copied >= length) throw std::runtime_error("Cannot read SSHHELPER_DATA_DIR");
        return fs::absolute(fs::path(std::wstring(value.data(), copied))).lexically_normal();
    }
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local)))
        throw std::runtime_error("Cannot locate the Windows user data directory");
    const fs::path base(local);
    CoTaskMemFree(local);
    return base / L"sshhelper";
}
std::filesystem::path defaultCredentialPath() { return dataDirectory() / L"sshhelper.credentials"; }
std::filesystem::path hostKeyPathForCredentials(const std::filesystem::path& credentials) {
    // --credentials keeps existing explicitly selected AirSFTP installations usable.
    const auto name = credentials.filename().wstring();
    const bool legacy = CompareStringOrdinal(name.c_str(), -1, L"AirSFTP.credentials", -1, TRUE) == CSTR_EQUAL;
    return credentials.parent_path() / (legacy ?
        L"AirSFTP.known_hosts" : L"sshhelper.known_hosts");
}
bool migrateLegacyData(const std::filesystem::path& sourceDirectory,
                       const std::filesystem::path& destinationDirectory, std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    std::error_code ec;
    const auto source = fs::absolute(sourceDirectory, ec).lexically_normal();
    if (ec) { error = "Cannot resolve legacy data directory"; return false; }
    const auto destination = fs::absolute(destinationDirectory, ec).lexically_normal();
    if (ec || source == destination) { error = "Choose a separate user data directory"; return false; }
    const auto sourceCredentials = source / (fs::exists(source / L"sshhelper.credentials", ec) ?
        L"sshhelper.credentials" : L"AirSFTP.credentials");
    if (ec) { error = "Cannot inspect legacy credentials"; return false; }
    ServerSettings checked;
    if (!fs::is_regular_file(sourceCredentials, ec) || ec) { error = "Legacy credentials not found"; return false; }
    if (!loadServerSettings(sourceCredentials, checked, error)) return false;
    const auto credentials = destination / L"sshhelper.credentials";
    const auto pins = destination / L"sshhelper.known_hosts";
    if (fs::exists(credentials, ec) || ec || fs::exists(pins, ec) || ec) {
        error = "Destination already contains credentials or host pins; migration will not overwrite them";
        return false;
    }
    fs::create_directories(destination, ec);
    if (ec) { error = "Cannot create user data directory: " + ec.message(); return false; }
    const auto sourcePins = hostKeyPathForCredentials(sourceCredentials);
    bool copiedPins = false;
    if (fs::exists(sourcePins, ec)) {
        if (ec || !fs::copy_file(sourcePins, pins, fs::copy_options::none, ec)) {
            error = "Cannot migrate host pins: " + ec.message(); return false;
        }
        copiedPins = true;
    } else if (ec) { error = "Cannot inspect legacy host pins: " + ec.message(); return false; }
    if (!fs::copy_file(sourceCredentials, credentials, fs::copy_options::none, ec)) {
        error = "Cannot migrate credentials: " + ec.message();
        if (copiedPins) { std::error_code ignored; fs::remove(pins, ignored); }
        return false;
    }
    // CopyFile may carry a read-only source attribute from Program Files.
    for (const auto& file : {credentials, pins}) {
        const auto attributes = GetFileAttributesW(file.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
            const auto writable = attributes & ~FILE_ATTRIBUTE_READONLY;
            if (!SetFileAttributesW(file.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL)) {
                error = "Migrated data could not be made writable"; return false;
            }
        }
    }
    return true;
}
