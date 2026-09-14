#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct SensorValue {
    std::string label;
    double value = 0.0;
};

struct GpuMetrics {
    std::string name;
    double temperature = -1.0;
    double powerWatts = -1.0;
    double fanPercent = -1.0;
    double fanRpm = -1.0;
    std::uint64_t memoryUsed = 0;
    std::uint64_t memoryTotal = 0;
};

struct TelemetrySnapshot {
    bool connected = false;
    std::string status = "Not connected";
    std::string fingerprint;
    std::string cpuName;
    std::string cpuPowerStrategy;
    double packageTemperature = -1.0;
    double packagePowerWatts = -1.0;
    std::vector<SensorValue> coreTemperatures;
    std::vector<SensorValue> fans;
    std::vector<GpuMetrics> gpus;
    std::uint64_t memoryUsed = 0;
    std::uint64_t memoryTotal = 0;
    std::uint64_t diskUsed = 0;
    std::uint64_t diskTotal = 0;
    std::uint64_t networkDown = 0;
    std::uint64_t networkUp = 0;
    double sshLatencyMs = 0.0;
    std::chrono::system_clock::time_point sampledAt{};
};

struct ConnectionConfig {
    std::string host;
    int port = 22;
    std::string username;
    std::string password;
    std::string privateKeyPath;
    std::string keyPassphrase;
};

enum class TransferDirection { Upload, Download };

struct TransferEntry {
    std::string relativePath;
    bool directory = false;
    std::uint64_t size = 0;
};

struct TransferProgress {
    bool active = false;
    bool completed = false;
    bool failed = false;
    TransferDirection direction = TransferDirection::Upload;
    std::string fileName;
    std::string message;
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    std::uint64_t bytesPerSecond = 0;
    std::size_t filesCompleted = 0;
    std::size_t filesTotal = 0;
};
