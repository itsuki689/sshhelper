#include "credential_store.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
        return __LINE__; \
    } \
} while (false)

// Independently encode the previous on-disk schema to test migration.
bool writeV2Profile(const std::filesystem::path& path, unsigned version = 2) {
    std::vector<unsigned char> plain;
    auto u32 = [&](std::uint32_t n) { for (int i=0;i<4;++i) plain.push_back(static_cast<unsigned char>(n >> (i*8))); };
    auto str = [&](const std::string& text) { u32(static_cast<std::uint32_t>(text.size())); plain.insert(plain.end(),text.begin(),text.end()); };
    u32(version); u32(0x064E); u32(0x0650); u32(1); u32(22);
    for (const auto* text : {"v2.example", "user", "test-password", "", "", "Legacy", "D:/data", "/tmp"}) str(text);
    u32(0xFFFFFF); u32(0x1D1D1F);
    if (version >= 3) u32(7);
    unsigned char salt[] = "AirSFTP/credentials/v1";
    // The original entropy includes its terminating null byte (23 bytes total).
    unsigned char entropyBytes[23]{};
    std::copy(std::begin(salt),std::end(salt),entropyBytes);
    DATA_BLOB input{static_cast<DWORD>(plain.size()),plain.data()}, output{}, entropy{23,entropyBytes};
    if (!CryptProtectData(&input,L"test",&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)) return false;
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    file.write("ASFTPDP1",8); file.write(reinterpret_cast<char*>(output.pbData),output.cbData); file.close();
    LocalFree(output.pbData);
    return static_cast<bool>(file);
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        (L"AirSFTP-credential-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
    std::filesystem::remove(path);

    ConnectionConfig source;
    source.host = "server.example";
    source.port = 2222;
    source.username = "air-user";
    source.password = "not-plain-text-secret";
    source.privateKeyPath = "C:\\keys\\id_ed25519";
    source.keyPassphrase = source.password;
    std::string error;
    CHECK(saveConnectionCredentials(path, source, error));

    std::ifstream file(path, std::ios::binary);
    CHECK(file);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
    file.close();
    CHECK(!bytes.empty());
    CHECK(std::search(bytes.begin(), bytes.end(), source.password.begin(), source.password.end()) == bytes.end());
    CHECK(std::search(bytes.begin(), bytes.end(), source.host.begin(), source.host.end()) == bytes.end());

    ConnectionConfig loaded;
    CHECK(loadConnectionCredentials(path, loaded, error));
    CHECK(loaded.host == source.host);
    CHECK(loaded.port == source.port);
    CHECK(loaded.username == source.username);
    CHECK(loaded.password == source.password);
    CHECK(loaded.privateKeyPath == source.privateKeyPath);
    CHECK(loaded.keyPassphrase == source.keyPassphrase);

    ServerSettings migrated;
    CHECK(loadServerSettings(path, migrated, error));
    CHECK(migrated.servers.size() == 1);
    CHECK(migrated.servers.front().refreshIntervalSeconds == 2);
    CHECK(migrated.servers.front().connection.password == source.password);
    CHECK(migrated.servers.front().name == source.host);
    ServerSettings multiple;
    ServerProfile first;
    first.connection = source;
    first.name = "工作服务器";
    first.localFolder = "D:/数据";
    first.remoteFolder = "/srv/data";
    first.refreshIntervalSeconds = 7;
    first.background = 0x123456;
    first.foreground = 0xF1F2F3;
    multiple.servers.push_back(first);
    auto second = first;
    second.connection.host = "second.example";
    second.connection.password = "second-secret";
    second.name = "Backup";
    second.refreshIntervalSeconds = 60;
    multiple.servers.push_back(second);
    CHECK(saveServerSettings(path, multiple, error));
    ServerSettings restored;
    CHECK(loadServerSettings(path, restored, error));
    CHECK(restored.servers.size() == 2);
    CHECK(restored.servers[0].refreshIntervalSeconds == 7);
    CHECK(restored.servers[1].refreshIntervalSeconds == 60);
    CHECK(restored.servers[0].name == first.name);
    CHECK(restored.servers[0].localFolder == first.localFolder);
    CHECK(restored.servers[0].remoteFolder == first.remoteFolder);
    CHECK(restored.servers[0].background == first.background);
    CHECK(restored.servers[0].foreground == first.foreground);
    CHECK(restored.servers[1].connection.host == second.connection.host);
    CHECK(restored.servers[1].connection.password == second.connection.password);
    for (auto interval : {0u, 3601u}) {
        auto invalidInterval = multiple;
        invalidInterval.servers[0].refreshIntervalSeconds = interval;
        CHECK(!saveServerSettings(path, invalidInterval, error));
    }
    auto invalid = multiple;
    invalid.servers[0].background = 0x1000000;
    CHECK(!saveServerSettings(path, invalid, error));
    CHECK(loadServerSettings(path, restored, error) && restored.servers.size() == 2);
    invalid = multiple;
    invalid.servers.resize(65);
    CHECK(!saveServerSettings(path, invalid, error));
    std::ifstream encryptedProfiles(path, std::ios::binary);
    const std::vector<char> profileBytes((std::istreambuf_iterator<char>(encryptedProfiles)), {});
    encryptedProfiles.close();
    CHECK(std::search(profileBytes.begin(), profileBytes.end(), second.connection.password.begin(),
        second.connection.password.end()) == profileBytes.end());
    CHECK(saveServerSettings(path, ServerSettings{}, error));
    CHECK(loadServerSettings(path, restored, error) && restored.servers.empty());

    CHECK(writeV2Profile(path));
    CHECK(loadServerSettings(path, restored, error));
    CHECK(restored.servers.size() == 1 && restored.servers[0].connection.host == "v2.example");
    CHECK(restored.servers[0].refreshIntervalSeconds == 2);

    CHECK(writeV2Profile(path, 3));
    CHECK(loadServerSettings(path, restored, error));
    CHECK(restored.servers.size() == 1 && !restored.servers[0].local);
    CHECK(restored.servers[0].refreshIntervalSeconds == 7);
    auto withLocal = multiple;
    ServerProfile local; local.local = true; local.name = "本机"; local.refreshIntervalSeconds = 5;
    local.background = 0x18212C;
    withLocal.servers.push_back(local);
    CHECK(saveServerSettings(path, withLocal, error));
    CHECK(loadServerSettings(path, restored, error));
    CHECK(restored.servers.size() == 3 && restored.servers[2].local && !restored.servers[0].local);
    CHECK(restored.servers[2].connection.host.empty() && restored.servers[2].refreshIntervalSeconds == 5);
    CHECK(restored.servers[2].background == 0x18212C);
    withLocal.servers.push_back(local);
    CHECK(!saveServerSettings(path, withLocal, error));

    auto damaged = profileBytes;
    damaged.back() ^= 0x5a;
    std::ofstream corrupted(path, std::ios::binary | std::ios::trunc);
    CHECK(corrupted);
    corrupted.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    corrupted.close();
    CHECK(corrupted);
    ConnectionConfig rejected;
    const bool corruptedLoaded = loadConnectionCredentials(path, rejected, error);
    CHECK(!corruptedLoaded);

    ServerSettings unchanged = multiple;
    CHECK(!loadServerSettings(path, unchanged, error));
    CHECK(unchanged.servers.size() == 2 && unchanged.servers.front().name == first.name);
    std::filesystem::remove(path);
    return 0;
}
