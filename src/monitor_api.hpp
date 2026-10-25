#pragma once

#include "models.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>

namespace monitor_api {
using Json = nlohmann::json;
struct Reading {
    TelemetrySnapshot snapshot;
    bool ratesReady = false;
};
// Sources are constructed, sampled and destroyed on their worker thread.
class Source {
public:
    virtual ~Source() = default;
    virtual Reading sample(std::stop_token stop) = 0;
};
using SourceFactory = std::function<std::unique_ptr<Source>(const ServerProfile&)>;

class Service {
public:
    explicit Service(std::filesystem::path credentials, SourceFactory factory = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Json listServers();
    Json getMonitoring(const Json& arguments);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string serialize(const Json& value);
}
