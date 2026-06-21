#include "src/plugin/command_handler_plugin.h"
#include "src/infra/logger.h"
#include "src/plugin/process_manager.h"
#include "src/plugin/plugin_base.h"

void CommandHandlerConfig::from_json(const json& j) {
    if (j.contains("command_prefix")) {
        command_prefix = j["command_prefix"];
    }
    if (j.contains("case_insensitive")) {
        case_insensitive = j["case_insensitive"];
    }
}

json CommandHandlerConfig::to_json() const {
    json j;
    j["command_prefix"] = command_prefix;
    j["case_insensitive"] = case_insensitive;
    return j;
}

// ──────────────────────────────────────────────────────
// CommandHandlerPlugin 实现
// ──────────────────────────────────────────────────────

CommandHandlerPlugin::CommandHandlerPlugin()
    : _config(std::make_shared<CommandHandlerConfig>()),
      _initialized(false) {
    Logger::get().info("[命令处理]", "插件对象创建");
}

CommandHandlerPlugin::~CommandHandlerPlugin() {
    Logger::get().info("[命令处理]", "插件对象销毁");
}

void CommandHandlerPlugin::on_load() {
    try {
        Logger::get().info("[命令处理]", "开始加载...");
        _initialized = true;
        Logger::get().info("[命令处理]", "插件加载完成");
    } catch (const std::exception& e) {
        Logger::get().error("[命令处理]", "加载失败: " + std::string(e.what()));
        throw;
    }
}

void CommandHandlerPlugin::on_unload() {
    try {
        Logger::get().info("[命令处理]", "开始卸载...");
        _initialized = false;
        Logger::get().info("[命令处理]", "插件卸载完成");
    } catch (const std::exception& e) {
        Logger::get().error("[命令处理]", "卸载失败: " + std::string(e.what()));
    }
}

std::vector<ToolDefinition> CommandHandlerPlugin::get_tools() const {
    std::vector<ToolDefinition> tools;

    tools.push_back(ToolDefinition{
        "system_info",
        "获取系统信息",
        {},
        [this](const json& params) { return const_cast<CommandHandlerPlugin*>(this)->handle_system_info(params); }
    });

    tools.push_back(ToolDefinition{
        "plugin_list",
        "列出已加载的插件及其工具",
        {},
        [this](const json& params) { return const_cast<CommandHandlerPlugin*>(this)->handle_plugin_list(params); }
    });

    tools.push_back(ToolDefinition{
        "list_all_tools",
        "列出所有可用工具",
        {},
        [this](const json& params) { return const_cast<CommandHandlerPlugin*>(this)->handle_list_all_tools(params); }
    });

    return tools;
}

void CommandHandlerPlugin::on_event(const Event& event) {
    // 可以监听系统事件
}

std::shared_ptr<PluginConfigBase> CommandHandlerPlugin::get_config() {
    return _config;
}

void CommandHandlerPlugin::set_config(const json& config_json) {
    if (_config) {
        _config->from_json(config_json);
        Logger::get().info("[命令处理]", "配置已更新");
    }
}

// ──────────────────────────────────────────────────────
// 工具实现
// ──────────────────────────────────────────────────────

ToolResult CommandHandlerPlugin::handle_system_info(const json& params) {
    try {
        auto& pm = ProcessManager::get();
        auto health = pm.check_health();

        json response;
        response["process_type"] = (pm.get_process_type() == ProcessType::RUNNER ? "Runner" : "Worker");
        response["status"] = health.status;
        response["uptime_seconds"] = (int)health.uptime_seconds;
        response["restart_count"] = pm.get_restart_count();
        response["is_healthy"] = health.is_healthy;

        return ToolResult{true, "system_info", "系统信息已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "system_info", std::string(e.what())};
    }
}

ToolResult CommandHandlerPlugin::handle_plugin_list(const json& params) {
    try {
        auto& mgr = PluginManager::get();
        auto tools = mgr.list_tools();

        json response;
        response["total_tools"] = (int)tools.size();
        response["tools"] = json::array();

        for (const auto& tool : tools) {
            response["tools"].push_back(tool);
        }

        return ToolResult{true, "plugin_list", "插件列表已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "plugin_list", std::string(e.what())};
    }
}

ToolResult CommandHandlerPlugin::handle_list_all_tools(const json& params) {
    try {
        auto& mgr = PluginManager::get();
        auto tools = mgr.list_tools();

        // 按工具名分类
        std::map<std::string, std::vector<std::string>> tools_by_prefix;
        for (const auto& tool : tools) {
            size_t sep = tool.find('_');
            std::string prefix = (sep != std::string::npos) ? tool.substr(0, sep) : "system";
            tools_by_prefix[prefix].push_back(tool);
        }

        json response;
        response["categories"] = json::object();

        for (const auto& [prefix, tool_list] : tools_by_prefix) {
            response["categories"][prefix] = json::array();
            for (const auto& tool : tool_list) {
                response["categories"][prefix].push_back(tool);
            }
        }

        response["total"] = (int)tools.size();

        return ToolResult{true, "list_all_tools", "工具列表已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "list_all_tools", std::string(e.what())};
    }
}

std::shared_ptr<MaiBotPlugin> create_command_handler_plugin() {
    return std::make_shared<CommandHandlerPlugin>();
}


