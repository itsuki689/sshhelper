#pragma once
#include "monitor_api.hpp"
#include <functional>
#include <istream>
#include <optional>
#include <ostream>

namespace sshhelper_mcp {
using Json = monitor_api::Json;
using ToolHandler = std::function<Json(const std::string&, const Json&)>;
class Server {
public:
    explicit Server(ToolHandler handler);
    std::optional<Json> handle(const Json& request);
    void serve(std::istream& input, std::ostream& output);
private:
    ToolHandler handler_;
    bool initialized_ = false;
};
}
