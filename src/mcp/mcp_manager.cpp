#include "src/mcp/mcp_manager.h"
#include "src/core/config.h"
#include "src/core/tools.h"
#include "mcp_sse_client.h"
#include "mcp_logger.h"
#include "mcp_tool.h"
#include <iostream>
#include <sstream>

using json = nlohmann::json;

std::string McpManager::makeQualifiedName(const std::string& server, const std::string& tool) {
    return server + ":" + tool;
}

void McpManager::setup(Tools& tools) {
    const auto& servers = Config::get().mcp_servers();

    for (const auto& s : servers) {
        if (!s.enabled) {
            LOG_WARNING(s.name, " 已跳过（disabled）");
            continue;
        }
        if (s.url.empty()) {
            LOG_WARNING(s.name, " URL 为空，跳过");
            continue;
        }

        std::string full = s.url;
        bool https = false;
        if (full.starts_with("https://")) { https = true; full = full.substr(8); }
        else if (full.starts_with("http://")) { full = full.substr(7); }

        auto pathPos = full.find('/');
        std::string hostPort = pathPos != std::string::npos ? full.substr(0, pathPos) : full;
        std::string sseEndpoint = pathPos != std::string::npos ? full.substr(pathPos) : "/sse";
        std::string schemeHost = std::string(https ? "https://" : "http://") + hostPort;

        auto client = std::make_unique<mcp::sse_client>(schemeHost, sseEndpoint, true, "");

        if (!s.api_key.empty()) {
            client->set_header("Authorization", "Bearer " + s.api_key);
        }
        for (const auto& [k, v] : s.headers) {
            if (!v.empty()) client->set_header(k, v);
        }

        LOG_INFO("连接 ", s.name, " (", s.url, ") ...");

        // 设置 capabilities
        client->set_capabilities({
            {"tools", json::object()}
        });

        if (!client->initialize("qbot", "1.0")) {
            LOG_ERROR(s.name, " 连接失败，跳过");
            continue;
        }

        auto mcpTools = client->get_tools();
        if (mcpTools.empty()) {
            LOG_WARNING(s.name, " 无可用工具");
        } else {
            LOG_INFO(s.name, " 加载 ", mcpTools.size(), " 个工具");
        }

        std::lock_guard<std::mutex> lock(_mutex);
        _clients.push_back(std::move(client));

        mcp::sse_client* rawClient = _clients.back().get();

        for (const auto& mt : mcpTools) {
            std::string qualified = makeQualifiedName(s.name, mt.name);
            RegisteredTool rt;
            rt.serverName = s.name;
            rt.toolName = mt.name;
            rt.description = mt.description;
            rt.parameters = mt.parameters_schema;
            rt.client = rawClient;
            _toolMap[qualified] = std::move(rt);

            json toolDef = {
                {"type", "function"},
                {"function", {
                    {"name", qualified},
                    {"description", "[MCP:" + rt.serverName + "] " + mt.description},
                    {"parameters", mt.parameters_schema}
                }}
            };

            tools.registerTool(qualified, toolDef,
                [this, qualified](const json& args) -> std::string {
                    return callTool(qualified, args);
                }
            );
        }
    }

    std::ostringstream ss; ss << _toolMap.size();
    LOG_INFO("初始化完成，共注册 " + ss.str() + " 个 MCP 工具");
}

std::vector<json> McpManager::getToolDefinitions() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<json> defs;
    for (const auto& [qname, rt] : _toolMap) {
        defs.push_back({
            {"type", "function"},
            {"function", {
                {"name", qname},
                {"description", "[MCP:" + rt.serverName + "] " + rt.description},
                {"parameters", rt.parameters}
            }}
        });
    }
    return defs;
}

std::string McpManager::callTool(const std::string& qualifiedName, const json& args) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _toolMap.find(qualifiedName);
    if (it == _toolMap.end()) return "[MCP] 未知工具: " + qualifiedName;

    mcp::sse_client* client = it->second.client;
    if (!client) return "[MCP] 连接已断开: " + it->second.serverName;

    try {
        mcp::json result = client->call_tool(it->second.toolName, args);
        if (result.is_array()) {
            std::string text;
            for (const auto& item : result) {
                if (item.value("type", "") == "text") text += item.value("text", "");
            }
            return text.empty() ? "[MCP 返回空结果]" : text;
        }
        if (result.is_object() && result.contains("text")) return result["text"];
        return result.dump();
    } catch (const std::exception& e) {
        return std::string("[MCP 调用异常] ") + e.what();
    }
}

json McpManager::listAllTools() const {
    std::lock_guard<std::mutex> lock(_mutex);
    json result = json::object();
    for (const auto& [qname, rt] : _toolMap) {
        result[qname] = {{"server", rt.serverName}, {"description", rt.description}};
    }
    return result;
}

McpManager::~McpManager() = default;
