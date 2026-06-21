#pragma once

#include"base.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include "mcp_client.h"

using json = nlohmann::json;

// MCP 管理器：读取配置、连接所有 server、注册工具到 Tools
class McpManager {
public:
    McpManager() = default;
    ~McpManager();

    // 禁止拷贝
    McpManager(const McpManager&) = delete;
    McpManager& operator=(const McpManager&) = delete;

    // 从 Config 加载 mcp_servers，连接每个 server，注册工具
    void setup(Tools& tools);

    // 获取所有已注册 MCP 工具的定义
    std::vector<json> getToolDefinitions() const;

    // 根据 qualified name (server:tool) 路由调用
    std::string callTool(const std::string& qualifiedName, const json& args);

    // 列出所有已连接 server 的工具（供 /mcp_tools 命令使用）
    json listAllTools() const;

private:
    struct RegisteredTool {
        std::string serverName;
        std::string toolName;
        std::string description;
        json parameters;
        std::weak_ptr<McpClient> client;
    };

    std::vector<std::shared_ptr<McpClient>> _clients;
    std::unordered_map<std::string, RegisteredTool> _toolMap; // qualifiedName → tool
    mutable std::mutex _mutex;

    static std::string makeQualifiedName(const std::string& server, const std::string& tool);
};
