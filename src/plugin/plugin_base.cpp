#include "src/plugin/plugin_base.h"
#include "src/infra/logger.h"

// ──────────────────────────────────────────────────────
// MaiBotPlugin 实现
// ──────────────────────────────────────────────────────

ToolResult MaiBotPlugin::invoke_tool(const std::string& name, const json& params) {
    auto it = _tools.find(name);
    if (it == _tools.end()) {
        return ToolResult{false, name, "Tool not found: " + name};
    }

    try {
        return it->second.handler(params);
    } catch (const std::exception& e) {
        return ToolResult{false, name, std::string("Tool execution failed: ") + e.what()};
    }
}

CommandResult MaiBotPlugin::handle_command(const CommandContext& ctx) {
    return CommandResult{false, "Command not implemented"};
}

// ──────────────────────────────────────────────────────
// PluginManager 实现
// ──────────────────────────────────────────────────────

PluginManager& PluginManager::get() {
    static PluginManager instance;
    return instance;
}

void PluginManager::register_plugin(const std::string& name, std::shared_ptr<MaiBotPlugin> plugin) {
    if (!plugin) {
        Logger::get().error("[插件]", "无法注册空指针插件: " + name);
        return;
    }

    _plugins[name] = plugin;
    Logger::get().info("[插件]", "已注册: " + name);

    // 注册该插件的所有工具
    auto tools = plugin->get_tools();
    for (const auto& tool : tools) {
        _tool_to_plugin[tool.name] = name;
        Logger::get().debug("[插件]", "注册工具: " + tool.name + " -> " + name);
    }
}

void PluginManager::unregister_plugin(const std::string& name) {
    auto it = _plugins.find(name);
    if (it == _plugins.end()) {
        Logger::get().warn("[插件]", "插件不存在: " + name);
        return;
    }

    // 删除该插件的所有工具映射
    auto tools = it->second->get_tools();
    for (const auto& tool : tools) {
        _tool_to_plugin.erase(tool.name);
    }

    _plugins.erase(it);
    Logger::get().info("[插件]", "已注销: " + name);
}

std::shared_ptr<MaiBotPlugin> PluginManager::get_plugin(const std::string& name) const {
    auto it = _plugins.find(name);
    if (it == _plugins.end()) {
        return nullptr;
    }
    return it->second;
}

bool PluginManager::load_plugin(const std::string& name) {
    auto plugin = get_plugin(name);
    if (!plugin) {
        Logger::get().error("[插件]", "插件不存在: " + name);
        return false;
    }

    try {
        plugin->on_load();
        Logger::get().info("[插件]", "已加载: " + name);

        // 广播插件加载事件
        Event event;
        event.type = EventType::PLUGIN_LOADED;
        event.data["plugin_name"] = name;
        event.timestamp = std::time(nullptr);
        broadcast_event(event);

        return true;
    } catch (const std::exception& e) {
        Logger::get().error("[插件]", "加载失败 " + name + ": " + std::string(e.what()));
        return false;
    }
}

bool PluginManager::unload_plugin(const std::string& name) {
    auto plugin = get_plugin(name);
    if (!plugin) {
        Logger::get().error("[插件]", "插件不存在: " + name);
        return false;
    }

    try {
        plugin->on_unload();
        Logger::get().info("[插件]", "已卸载: " + name);

        // 广播插件卸载事件
        Event event;
        event.type = EventType::PLUGIN_UNLOADED;
        event.data["plugin_name"] = name;
        event.timestamp = std::time(nullptr);
        broadcast_event(event);

        return true;
    } catch (const std::exception& e) {
        Logger::get().error("[插件]", "卸载失败 " + name + ": " + std::string(e.what()));
        return false;
    }
}

ToolResult PluginManager::call_tool(const std::string& tool_name, const json& params) {
    auto it = _tool_to_plugin.find(tool_name);
    if (it == _tool_to_plugin.end()) {
        Logger::get().warn("[工具]", "工具不存在: " + tool_name);
        return ToolResult{false, tool_name, "Tool not found: " + tool_name};
    }

    auto plugin = get_plugin(it->second);
    if (!plugin) {
        return ToolResult{false, tool_name, "Plugin not found for tool: " + tool_name};
    }

    Logger::get().debug("[工具]", "调用: " + tool_name);
    return plugin->invoke_tool(tool_name, params);
}

std::vector<std::string> PluginManager::list_tools() const {
    std::vector<std::string> tools;
    for (const auto& [tool_name, _] : _tool_to_plugin) {
        tools.push_back(tool_name);
    }
    return tools;
}

void PluginManager::broadcast_event(const Event& event) {
    for (auto& [name, plugin] : _plugins) {
        try {
            plugin->on_event(event);
        } catch (const std::exception& e) {
            Logger::get().error("[事件]", "插件 " + name + " 处理事件失败: " + std::string(e.what()));
        }
    }
}

CommandResult PluginManager::execute_command(const CommandContext& ctx) {
    // 这里可以实现命令路由逻辑
    // 简单起见，先返回未实现
    return CommandResult{false, "Command routing not implemented"};
}
