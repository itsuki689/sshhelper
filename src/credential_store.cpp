#include "credential_store.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <utility>
#include <vector>

namespace {
constexpr std::array<unsigned char, 8> Magic{'A', 'S', 'F', 'T', 'P', 'D', 'P', '1'};
constexpr std::array<unsigned char, 23> Entropy{'A','i','r','S','F','T','P','/','c','r','e','d','e','n','t','i','a','l','s','/','v','1','\0'};
constexpr std::uint32_t Version = 1;
constexpr std::uintmax_t MaxCredentialFileSize = 1024 * 1024;

void appendU32(std::vector<unsigned char>& output, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<unsigned char>((value >> shift) & 0xff));
}

void appendString(std::vector<unsigned char>& output, const std::string& value) {
    appendU32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

bool readU32(const unsigned char* data, std::size_t size, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > size) return false;
    value = 0;
    for (int shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(data[offset++]) << shift;
    return true;
}

bool readString(const unsigned char* data, std::size_t size, std::size_t& offset, std::string& value) {
    std::uint32_t length = 0;
    if (!readU32(data, size, offset, length) || length > size - offset) return false;
    value.assign(reinterpret_cast<const char*>(data + offset), length);
    offset += length;
    return true;
}

DATA_BLOB entropyBlob() {
    return DATA_BLOB{static_cast<DWORD>(Entropy.size()),
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(Entropy.data()))};
}

void clearBytes(std::vector<unsigned char>& bytes) {
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
}

std::string windowsError(const char* prefix) {
    return std::string(prefix) + " (Windows error " + std::to_string(GetLastError()) + ")";
}
}

std::filesystem::path defaultCredentialPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
        return std::filesystem::path(buffer.data()).parent_path() / L"AirSFTP.credentials";
    return std::filesystem::current_path() / L"AirSFTP.credentials";
}

bool saveConnectionCredentials(const std::filesystem::path& path,
                               const ConnectionConfig& config, std::string& error) {
    error.clear();
    std::vector<unsigned char> plain;
    appendU32(plain, Version);
    appendU32(plain, static_cast<std::uint32_t>(config.port));
    appendString(plain, config.host);
    appendString(plain, config.username);
    appendString(plain, config.password);
    appendString(plain, config.privateKeyPath);

    DATA_BLOB input{static_cast<DWORD>(plain.size()), plain.data()};
    DATA_BLOB output{};
    DATA_BLOB entropy = entropyBlob();
    if (!CryptProtectData(&input, L"AirSFTP SSH credentials", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = windowsError("Cannot encrypt saved SSH credentials");
        clearBytes(plain);
        return false;
    }

    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (file) {
        file.write(reinterpret_cast<const char*>(Magic.data()), Magic.size());
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
        file.close();
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    clearBytes(plain);
    if (!file) {
        error = "Cannot write portable credential store";
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = windowsError("Cannot replace portable credential store");
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool loadConnectionCredentials(const std::filesystem::path& path,
                               ConnectionConfig& config, std::string& error) {
    error.clear();
    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError)) {
        if (!fileError) return true;
        error = "Cannot access saved credentials: " + fileError.message();
        return false;
    }
    const auto fileSize = std::filesystem::file_size(path, fileError);
    if (fileError || fileSize <= Magic.size() || fileSize > MaxCredentialFileSize) {
        error = "Saved SSH credentials are invalid";
        return false;
    }
    std::vector<unsigned char> encrypted(static_cast<std::size_t>(fileSize));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(encrypted.data()), static_cast<std::streamsize>(encrypted.size()));
    if (!file || !std::equal(Magic.begin(), Magic.end(), encrypted.begin())) {
        error = "Saved SSH credentials are invalid";
        return false;
    }

    DATA_BLOB input{static_cast<DWORD>(encrypted.size() - Magic.size()), encrypted.data() + Magic.size()};
    DATA_BLOB output{};
    DATA_BLOB entropy = entropyBlob();
    LPWSTR description = nullptr;
    if (!CryptUnprotectData(&input, &description, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = windowsError("Cannot decrypt saved SSH credentials for this Windows user");
        if (description) LocalFree(description);
        return false;
    }
    if (description) LocalFree(description);

    ConnectionConfig loaded;
    std::size_t offset = 0;
    std::uint32_t version = 0;
    std::uint32_t port = 0;
    const bool valid = readU32(output.pbData, output.cbData, offset, version) && version == Version &&
        readU32(output.pbData, output.cbData, offset, port) && port >= 1 && port <= 65535 &&
        readString(output.pbData, output.cbData, offset, loaded.host) &&
        readString(output.pbData, output.cbData, offset, loaded.username) &&
        readString(output.pbData, output.cbData, offset, loaded.password) &&
        readString(output.pbData, output.cbData, offset, loaded.privateKeyPath) &&
        offset == output.cbData;
    if (valid) {
        loaded.port = static_cast<int>(port);
        loaded.keyPassphrase = loaded.privateKeyPath.empty() ? std::string{} : loaded.password;
        config = std::move(loaded);
    } else {
        error = "Saved SSH credentials are invalid";
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return valid;
}

bool saveServerSettings(const std::filesystem::path& path,
                               const ServerSettings& settings, std::string& error) {
    error.clear();
    std::vector<unsigned char> plain;
    if (settings.servers.size() > 64) { error = "Maximum 64 servers"; return false; }
    appendU32(plain, 2);
    appendU32(plain, settings.nextHotkey);
    appendU32(plain, settings.previousHotkey);
    appendU32(plain, static_cast<std::uint32_t>(settings.servers.size()));
    for (const auto& profile : settings.servers) {
        const auto& config = profile.connection;
        if (config.port < 1 || config.port > 65535 || profile.background > 0xFFFFFF ||
            profile.foreground > 0xFFFFFF) { clearBytes(plain); error = "Invalid server settings"; return false; }
        appendU32(plain, config.port);
        for (const auto* value : {&config.host, &config.username, &config.password,
             &config.privateKeyPath, &config.keyPassphrase, &profile.name, &profile.localFolder, &profile.remoteFolder}) {
            if (value->size() > 65536) { clearBytes(plain); error = "Server setting too long"; return false; }
            appendString(plain, *value);
        }
        appendU32(plain, profile.background);
        appendU32(plain, profile.foreground);
    }
    if (plain.size() > MaxCredentialFileSize - 4096) {
        clearBytes(plain); error = "Server settings exceed size limit"; return false;
    }
    DATA_BLOB input{static_cast<DWORD>(plain.size()), plain.data()};
    DATA_BLOB output{};
    DATA_BLOB entropy = entropyBlob();
    if (!CryptProtectData(&input, L"AirSFTP SSH credentials", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = windowsError("Cannot encrypt saved SSH credentials");
        clearBytes(plain);
        return false;
    }

    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (file) {
        file.write(reinterpret_cast<const char*>(Magic.data()), Magic.size());
        file.write(reinterpret_cast<const char*>(output.pbData), output.cbData);
        file.close();
    }
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    clearBytes(plain);
    if (!file) {
        error = "Cannot write portable credential store";
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = windowsError("Cannot replace portable credential store");
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}


bool loadServerSettings(const std::filesystem::path& path,
                               ServerSettings& settings, std::string& error) {
    error.clear();
    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError)) {
        if (!fileError) return true;
        error = "Cannot access saved credentials: " + fileError.message();
        return false;
    }
    const auto fileSize = std::filesystem::file_size(path, fileError);
    if (fileError || fileSize <= Magic.size() || fileSize > MaxCredentialFileSize) {
        error = "Saved SSH credentials are invalid";
        return false;
    }
    std::vector<unsigned char> encrypted(static_cast<std::size_t>(fileSize));
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char*>(encrypted.data()), static_cast<std::streamsize>(encrypted.size()));
    if (!file || !std::equal(Magic.begin(), Magic.end(), encrypted.begin())) {
        error = "Saved SSH credentials are invalid";
        return false;
    }

    DATA_BLOB input{static_cast<DWORD>(encrypted.size() - Magic.size()), encrypted.data() + Magic.size()};
    DATA_BLOB output{};
    DATA_BLOB entropy = entropyBlob();
    LPWSTR description = nullptr;
    if (!CryptUnprotectData(&input, &description, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        error = windowsError("Cannot decrypt saved SSH credentials for this Windows user");
        if (description) LocalFree(description);
        return false;
    }
    if (description) LocalFree(description);

    ServerSettings loaded;
    std::size_t offset = 0;
    std::uint32_t version = 0, count = 0;
    bool valid = readU32(output.pbData, output.cbData, offset, version);
    if (valid && version == 1) {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        ConnectionConfig legacy;
        if (!loadConnectionCredentials(path, legacy, error)) return false;
        ServerProfile profile;
        profile.connection = std::move(legacy);
        profile.name = profile.connection.host;
        loaded.servers.push_back(std::move(profile));
        settings = std::move(loaded);
        return true;
    }
    valid = valid && version == 2 &&
        readU32(output.pbData, output.cbData, offset, loaded.nextHotkey) &&
        readU32(output.pbData, output.cbData, offset, loaded.previousHotkey) &&
        readU32(output.pbData, output.cbData, offset, count) && count <= 64;
    for (std::uint32_t i = 0; valid && i < count; ++i) {
        ServerProfile profile;
        auto& config = profile.connection;
        std::uint32_t port = 0;
        valid = readU32(output.pbData, output.cbData, offset, port) && port >= 1 && port <= 65535;
        config.port = static_cast<int>(port);
        for (auto* value : {&config.host, &config.username, &config.password, &config.privateKeyPath,
             &config.keyPassphrase, &profile.name, &profile.localFolder, &profile.remoteFolder})
            valid = valid && readString(output.pbData, output.cbData, offset, *value) && value->size() <= 65536;
        valid = valid && readU32(output.pbData, output.cbData, offset, profile.background) &&
            readU32(output.pbData, output.cbData, offset, profile.foreground) &&
            profile.background <= 0xFFFFFF && profile.foreground <= 0xFFFFFF;
        if (valid) loaded.servers.push_back(std::move(profile));
    }
    valid = valid && offset == output.cbData;
    if (valid) settings = std::move(loaded);
    else error = "Saved server settings are invalid";
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return valid;
}
