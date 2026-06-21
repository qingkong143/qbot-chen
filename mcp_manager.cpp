#include"mcp_manager.h"
#include"config.h"
#include"tools.h"
#include <iostream>
#include <sstream>

using json = nlohmann::json;

// ── 构造 qualified name: "serverName:toolName" ───────────────────
std::string McpManager::makeQualifiedName(const std::string& server, const std::string& tool) {
    return server + ":" + tool;
}

// ── setup：连接所有 MCP server，注册工具 ─────────────────────────
void McpManager::setup(Tools& tools) {
    const auto& servers = Config::get().mcp_servers();

    for (const auto& s : servers) {
        if (!s.enabled) {
            std::cout << CLR_YELLOW "[MCP] " << s.name << " 已跳过（disabled）" << CLR_RESET << std::endl;
            continue;
        }

        if (s.url.empty()) {
            std::cout << CLR_YELLOW "[MCP] " << s.name << " URL 为空，跳过" << CLR_RESET << std::endl;
            continue;
        }

        McpServerConfig cfg;
        cfg.name = s.name;
        cfg.url = s.url;
        cfg.transport = s.transport;
        cfg.api_key = s.api_key;
        cfg.headers = s.headers;

        auto client = std::make_shared<McpClient>(std::move(cfg));

        std::cout << CLR_CYAN "[MCP] 连接 " << s.name << " (" << s.url << ") ..." << CLR_RESET << std::endl;

        if (!client->initialize()) {
            std::cout << CLR_RED "[MCP] " << s.name << " 连接失败，跳过" << CLR_RESET << std::endl;
            continue;
        }

        auto mcpTools = client->listTools();
        if (mcpTools.empty()) {
            std::cout << CLR_YELLOW "[MCP] " << s.name << " 无可用工具" << CLR_RESET << std::endl;
        } else {
            std::cout << CLR_GREEN "[MCP] " << s.name << " 加载 " << mcpTools.size()
                << " 个工具" << CLR_RESET << std::endl;
        }

        std::lock_guard<std::mutex> lock(_mutex);
        _clients.push_back(client);

        for (const auto& mt : mcpTools) {
            std::string qualified = makeQualifiedName(mt.server, mt.name);

            RegisteredTool rt;
            rt.serverName = mt.server;
            rt.toolName = mt.name;
            rt.description = mt.description;
            rt.parameters = mt.parameters;
            rt.client = client;

            _toolMap[qualified] = std::move(rt);

            // 注册到 Tools 系统
            std::string displayName = qualified;
            json def = {
                {"type", "function"},
                {"function", {
                    {"name", displayName},
                    {"description", "[MCP:" + mt.server + "] " + mt.description},
                    {"parameters", mt.parameters}
                }}
            };

            // handler 用 shared_ptr 捕获，保证 client 生命周期
            auto weakClient = std::weak_ptr<McpClient>(client);
            tools.registerTool(displayName, def,
                [this, qualified, weakClient](const json& args) -> std::string {
                    auto sp = weakClient.lock();
                    if (!sp) return "[MCP] 连接已断开";
                    return callTool(qualified, args);
                }
            );
        }
    }

    std::cout << CLR_GREEN "[MCP] 初始化完成，共注册 "
        << _toolMap.size() << " 个 MCP 工具" << CLR_RESET << std::endl;
}

// ── 获取所有工具定义 ──────────────────────────────────────────────
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

// ── 路由调用 ──────────────────────────────────────────────────────
std::string McpManager::callTool(const std::string& qualifiedName, const json& args) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _toolMap.find(qualifiedName);
    if (it == _toolMap.end()) {
        return "[MCP] 未知工具: " + qualifiedName;
    }

    auto sp = it->second.client.lock();
    if (!sp) {
        return "[MCP] 连接已断开: " + it->second.serverName;
    }

    return sp->callTool(it->second.toolName, args);
}

// ── 列出所有工具（供命令使用） ────────────────────────────────────
json McpManager::listAllTools() const {
    std::lock_guard<std::mutex> lock(_mutex);
    json result = json::object();
    for (const auto& [qname, rt] : _toolMap) {
        result[qname] = {
            {"server", rt.serverName},
            {"description", rt.description}
        };
    }
    return result;
}

// ── 析构 ──────────────────────────────────────────────────────────
McpManager::~McpManager() = default;
