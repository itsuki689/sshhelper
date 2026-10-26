#include "app_paths.hpp"
#include "credential_store.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n" << error << '\n'; return __LINE__; } } while(false)
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
int main() {
    namespace fs = std::filesystem;
    std::string error;
    const auto root = fs::temp_directory_path() / (L"sshhelper-\u6570\u636e test-" + std::to_wstring(GetCurrentProcessId()));
    CHECK(fs::create_directory(root));
    struct Cleanup { fs::path root; ~Cleanup() {
        for (const auto& entry : fs::recursive_directory_iterator(root))
            if (entry.is_regular_file()) SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code ec; fs::remove_all(root, ec);
    } } cleanup{root};
    const auto user = root / L"user data";
    CHECK(SetEnvironmentVariableW(L"SSHHELPER_DATA_DIR", user.c_str()));
    CHECK(dataDirectory() == user && defaultCredentialPath() == user / L"sshhelper.credentials");
    CHECK(!fs::exists(user)); // Reading paths, including from MCP, creates nothing.
    CHECK(dataDirectory(root / "explicit") == root / "explicit");
    CHECK(hostKeyPathForCredentials(defaultCredentialPath()) == user / "sshhelper.known_hosts");
    CHECK(hostKeyPathForCredentials(root / "AirSFTP.credentials") == root / "AirSFTP.known_hosts");

    ServerSettings settings;
    ServerProfile profile; profile.name = "Legacy"; profile.connection.host = "example.invalid";
    profile.connection.password = "fixture-only";
    settings.servers.push_back(profile);
    const auto legacy = root / "protected program";
    const auto original = legacy / "AirSFTP.credentials";
    CHECK(saveServerSettings(original, settings, error));
    std::ofstream(legacy / "AirSFTP.known_hosts", std::ios::binary) << "example.invalid:22 SHA256 original\r\n";
    const auto encrypted = contents(original);
    CHECK(SetFileAttributesW(original.c_str(), FILE_ATTRIBUTE_READONLY));
    CHECK(SetFileAttributesW((legacy / "AirSFTP.known_hosts").c_str(), FILE_ATTRIBUTE_READONLY));
    CHECK(migrateLegacyData(legacy, user, error));
    CHECK(contents(original) == encrypted && contents(user / "sshhelper.credentials") == encrypted);
    CHECK(contents(user / "sshhelper.known_hosts") == "example.invalid:22 SHA256 original\r\n");
    CHECK(GetFileAttributesW(original.c_str()) & FILE_ATTRIBUTE_READONLY);
    CHECK(!(GetFileAttributesW((user / "sshhelper.credentials").c_str()) & FILE_ATTRIBUTE_READONLY));
    ServerSettings loaded;
    CHECK(loadServerSettings(defaultCredentialPath(), loaded, error));
    CHECK(loaded.servers.size() == 1 && loaded.servers[0].connection.password == "fixture-only");
    CHECK(saveServerSettings(defaultCredentialPath(), loaded, error));
    const auto before = contents(defaultCredentialPath());
    CHECK(!migrateLegacyData(legacy, user, error));
    CHECK(contents(defaultCredentialPath()) == before);
    const auto bad = root / "corrupt";
    fs::create_directory(bad); std::ofstream(bad / "AirSFTP.credentials") << "corrupt";
    CHECK(!migrateLegacyData(bad, root / "unchanged", error));
    CHECK(!fs::exists(root / "unchanged"));
    // A default data path never falls back to the executable's protected directory.
    CHECK(defaultCredentialPath().parent_path() != executableDirectory());
    CHECK(SetEnvironmentVariableW(L"SSHHELPER_DATA_DIR", nullptr));
    CHECK(defaultCredentialPath().parent_path().filename() == L"sshhelper");
    std::cout << "User paths, read-only lookup, explicit override, writable migration and no overwrite passed\n";
}
