#pragma once
#include <filesystem>
#include <string>

// Resolution is read-only. Directories are created only when data is saved.
std::filesystem::path executableDirectory();
std::filesystem::path dataDirectory(const std::filesystem::path& requested = {});
std::filesystem::path defaultCredentialPath();
std::filesystem::path hostKeyPathForCredentials(const std::filesystem::path& credentials);
bool migrateLegacyData(const std::filesystem::path& sourceDirectory,
                       const std::filesystem::path& destinationDirectory, std::string& error);
