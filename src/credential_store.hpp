#pragma once

#include "models.hpp"

#include <filesystem>
#include <string>

std::filesystem::path defaultCredentialPath();
bool saveServerSettings(const std::filesystem::path& path, const ServerSettings& settings, std::string& error);
bool loadServerSettings(const std::filesystem::path& path, ServerSettings& settings, std::string& error);
bool saveConnectionCredentials(const std::filesystem::path& path,
                               const ConnectionConfig& config, std::string& error);
bool loadConnectionCredentials(const std::filesystem::path& path,
                               ConnectionConfig& config, std::string& error);
