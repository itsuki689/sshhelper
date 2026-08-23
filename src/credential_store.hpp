#pragma once

#include "models.hpp"

#include <filesystem>
#include <string>

std::filesystem::path defaultCredentialPath();
bool saveConnectionCredentials(const std::filesystem::path& path,
                               const ConnectionConfig& config, std::string& error);
bool loadConnectionCredentials(const std::filesystem::path& path,
                               ConnectionConfig& config, std::string& error);
