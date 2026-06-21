#pragma once

#include "src/core/base.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <memory>

using json = nlohmann::json;

#include <map>

// MCP Server 配置
struct McpServerConfig {
    std::string name;                          // 唯一标识（如 "fetch"）
    std::string url;                           // 服务器地址
    std::string transport = "streamable_http"; // stdio | streamable_http | sse
    std::string api_key;                       // 可选：Bearer token（留空则不加）
    std::map<std::string, std::string> headers; // 任意自定义 HTTP 头
    bool enabled = true;
};

// 单个 MCP 工具定义（与 OpenAI function calling 格式对齐）
struct McpTool {
    std::string name;
    std::string description;
    json parameters;   // JSON Schema
    std::string server; // 来源 server 名
};

// MCP 客户端：通过 JSON-RPC 2.0 与远程 MCP Server 通信
class McpClient {
public:
    explicit McpClient(McpServerConfig cfg);
    ~McpClient();

    // 连接并初始化（发送 initialize + initialized）
    bool initialize();

    // 拉取工具列表
    std::vector<McpTool> listTools();

    // 调用远程工具
    std::string callTool(const std::string& toolName, const json& args);

    const std::string& name() const { return _cfg.name; }
    bool isConnected() const { return _initialized; }

private:
    // JSON-RPC 请求/响应
    json sendRequest(const std::string& method, const json& params = json::object());

    // HTTP POST（streamable HTTP transport）
    std::string httpPost(const std::string& path, const json& body);

    McpServerConfig _cfg;
    int _requestId = 1;
    mutable std::mutex _mutex;
    bool _initialized = false;
    std::string _sessionId; // mcp-session-id
};
