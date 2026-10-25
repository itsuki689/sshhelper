#include "mcp_server.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n"; return __LINE__; } } while(false)
using sshhelper_mcp::Json;
Json request(int id, const std::string& method, Json params = Json::object()) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}
Json initialize(int id = 1) {
    return request(id, "initialize", {{"protocolVersion", "2025-06-18"},
        {"capabilities", Json::object()}, {"clientInfo", {{"name", "test"}, {"version", "1"}}}});
}
int main() {
    unsigned calls = 0;
    sshhelper_mcp::Server server([&](const std::string& name, const Json& args) -> Json {
        ++calls;
        if (args.contains("fail")) throw std::runtime_error("Saved configuration unavailable");
        if (args.contains("bad")) throw std::invalid_argument("Invalid selector");
        return {{"tool", name}, {"servers", Json::array()}, {"host_name", "中文主机"}};
    });
    CHECK(server.handle(request(1, "tools/list"))->at("error")["code"] == -32002);
    CHECK(server.handle(request(1, "initialize"))->at("error")["code"] == -32602);
    CHECK(server.handle(initialize())->at("result")["serverInfo"]["name"] == "sshhelper-tool");
    CHECK(!server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
    const auto discovery = server.handle(request(2, "tools/list"))->at("result")["tools"];
    CHECK(discovery.size() == 2 && calls == 0);
    CHECK(discovery[1]["annotations"]["readOnlyHint"] == true);
    const auto result = server.handle(request(3, "tools/call", {{"name", "get_monitoring"}}))->at("result");
    CHECK(result["isError"] == false && calls == 1);
    CHECK(Json::parse(result["content"][0]["text"].get<std::string>()) == result["structuredContent"]);
    CHECK(server.handle(request(4, "tools/call", {{"name", "get_monitoring"}, {"arguments", {{"fail", true}}}}))->at("result")["isError"] == true);
    CHECK(server.handle(request(5, "tools/call", {{"name", "get_monitoring"}, {"arguments", {{"bad", true}}}}))->at("error")["code"] == -32602);
    CHECK(server.handle(request(6, "tools/call", {{"name", "execute"}}))->at("error")["code"] == -32602);
    CHECK(server.handle(request(7, "tools/call", {{"name", "list_servers"}, {"arguments", {{"extra", 1}}}}))->at("error")["code"] == -32602);
    CHECK(server.handle(request(8, "unknown"))->at("error")["code"] == -32601);
    CHECK(server.handle({{"jsonrpc", "2.0"}, {"id", false}, {"method", "ping"}})->at("error")["code"] == -32600);
    CHECK(server.handle(request(9, "ping"))->at("result").empty());
    CHECK(!server.handle({{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 8}}}}));
    // Framing, malformed data, Unicode and bounded memory; the next request must still work.
    std::stringstream input, output;
    input << "not json\n[]\n" << std::string(1024 * 1024 + 1, 'x') << '\n'
          << monitor_api::serialize(request(10, "ping")) << "\r\n"
          << monitor_api::serialize(request(11, "tools/call", {{"name", "list_servers"}}));
    server.serve(input, output);
    std::string line;
    std::getline(output, line); CHECK(Json::parse(line)["error"]["code"] == -32700);
    std::getline(output, line); CHECK(Json::parse(line)["error"]["code"] == -32600);
    std::getline(output, line); CHECK(Json::parse(line)["error"]["code"] == -32600);
    std::getline(output, line); CHECK(Json::parse(line)["id"] == 10);
    std::getline(output, line); CHECK(Json::parse(line)["result"]["structuredContent"]["host_name"] == "中文主机");
    CHECK(!std::getline(output, line));
    std::cout << "MCP lifecycle, schemas, errors, notifications and stdio framing passed\n";
    return 0;
}
