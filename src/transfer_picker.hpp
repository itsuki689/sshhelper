#pragma once
#include "models.hpp"
#include <windows.h>

// Modal selection UI; folder enumeration runs on a cancellable worker.
bool chooseTransferEntries(HWND owner, const ConnectionConfig& config,
                           TransferDirection direction, const std::string& sourceFolder,
                           std::vector<std::string>& selected);
