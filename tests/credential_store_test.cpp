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

    auto damaged = bytes;
    damaged.back() ^= 0x5a;
    std::ofstream corrupted(path, std::ios::binary | std::ios::trunc);
    CHECK(corrupted);
    corrupted.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    corrupted.close();
    CHECK(corrupted);
    ConnectionConfig rejected;
    const bool corruptedLoaded = loadConnectionCredentials(path, rejected, error);
    CHECK(!corruptedLoaded);

    std::filesystem::remove(path);
    return 0;
}
