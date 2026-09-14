#include "credential_store.hpp"

#include <windows.h>

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
    CHECK(migrated.servers.front().connection.password == source.password);
    CHECK(migrated.servers.front().name == source.host);
    ServerSettings multiple;
    ServerProfile first;
    first.connection = source;
    first.name = "工作服务器";
    first.localFolder = "D:/数据";
    first.remoteFolder = "/srv/data";
    first.background = 0x123456;
    first.foreground = 0xF1F2F3;
    multiple.servers.push_back(first);
    auto second = first;
    second.connection.host = "second.example";
    second.connection.password = "second-secret";
    second.name = "Backup";
    multiple.servers.push_back(second);
    multiple.nextHotkey = 0x064A;
    multiple.previousHotkey = 0x064B;
    CHECK(saveServerSettings(path, multiple, error));
    ServerSettings restored;
    CHECK(loadServerSettings(path, restored, error));
    CHECK(restored.servers.size() == 2);
    CHECK(restored.servers[0].name == first.name);
    CHECK(restored.servers[0].localFolder == first.localFolder);
    CHECK(restored.servers[0].remoteFolder == first.remoteFolder);
    CHECK(restored.servers[0].background == first.background);
    CHECK(restored.servers[0].foreground == first.foreground);
    CHECK(restored.servers[1].connection.host == second.connection.host);
    CHECK(restored.servers[1].connection.password == second.connection.password);
    CHECK(restored.nextHotkey == multiple.nextHotkey && restored.previousHotkey == multiple.previousHotkey);
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
