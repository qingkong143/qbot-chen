#pragma once

#include "src/core/base.h"
#include "mcp_sse_client.h"
#include "mcp_tool.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

using json = nlohmann::json;

// 前置声明
class Tools;

class McpManager {
public:
    McpManager() = default;
    ~McpManager();

    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    void setup(Tools& tools);
    std::vector<json> getToolDefinitions() const;
    std::string callTool(const std::string& qualifiedName, const json& args);
    json listAllTools() const;

private:
    struct RegisteredTool {
        std::string serverName;
        std::string toolName;
        std::string description;
        json parameters;
        mcp::sse_client* client;  // raw ptr, 生命周期由 _clients vector 管理
    };

    std::vector<std::unique_ptr<mcp::sse_client>> _clients;
    std::unordered_map<std::string, RegisteredTool> _toolMap;
    mutable std::mutex _mutex;

    static std::string makeQualifiedName(const std::string& server, const std::string& tool);
};
