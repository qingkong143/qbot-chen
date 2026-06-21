#pragma once

#include "src/core/base.h"
#include <map>
#include <memory>
#include <functional>
#include <any>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 工具参数类型定义
// ──────────────────────────────────────────────────────

enum class ToolParamType {
    STRING,
    INTEGER,
    FLOAT,
    BOOLEAN,
    ARRAY,
    OBJECT
};

struct ToolParameter {
    std::string name;
    ToolParamType type;
    std::string description;
    bool required = true;
    std::any default_value;
};

// 工具调用结果
struct ToolResult {
    bool success = true;
    std::string name;
    std::string content;
    std::map<std::string, std::any> data;
};

// 工具定义
struct ToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolParameter> parameters;
    std::function<ToolResult(const json&)> handler;
};

// ──────────────────────────────────────────────────────
// 事件定义
// ──────────────────────────────────────────────────────

enum class EventType {
    MESSAGE_RECEIVED,
    MESSAGE_SENT,
    USER_JOINED,
    USER_LEFT,
    PLUGIN_LOADED,
    PLUGIN_UNLOADED,
    CUSTOM
};

struct Event {
    EventType type;
    std::map<std::string, std::any> data;
    int64_t timestamp;
};

// ──────────────────────────────────────────────────────
// 命令定义
// ──────────────────────────────────────────────────────

struct CommandContext {
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::any> context;
};

struct CommandResult {
    bool success = true;
    std::string message;
    std::any data;
};

// ──────────────────────────────────────────────────────
// 配置基类
// ──────────────────────────────────────────────────────

class PluginConfigBase {
public:
    virtual ~PluginConfigBase() = default;
    virtual void from_json(const json& j) = 0;
    virtual json to_json() const = 0;
};

// ──────────────────────────────────────────────────────
// 插件基类
// ──────────────────────────────────────────────────────

class MaiBotPlugin {
public:
    virtual ~MaiBotPlugin() = default;

    // 生命周期钩子
    virtual void on_load() {}
    virtual void on_unload() {}

    // 获取插件信息
    virtual std::string get_name() const = 0;
    virtual std::string get_version() const { return "1.0.0"; }
    virtual std::string get_description() const { return ""; }

    // 工具接口
    virtual std::vector<ToolDefinition> get_tools() const { return {}; }
    ToolResult invoke_tool(const std::string& name, const json& params);

    // 事件处理
    virtual void on_event(const Event& event) {}

    // 命令处理
    virtual CommandResult handle_command(const CommandContext& ctx);

    // 配置接口
    virtual std::shared_ptr<PluginConfigBase> get_config() { return nullptr; }
    virtual void set_config(const json& config_json) {}

protected:
    std::map<std::string, ToolDefinition> _tools;
};

// ──────────────────────────────────────────────────────
// 插件管理器
// ──────────────────────────────────────────────────────

class PluginManager {
public:
    static PluginManager& get();

    // 插件生命周期管理
    void register_plugin(const std::string& name, std::shared_ptr<MaiBotPlugin> plugin);
    void unregister_plugin(const std::string& name);
    std::shared_ptr<MaiBotPlugin> get_plugin(const std::string& name) const;

    // 加载/卸载
    bool load_plugin(const std::string& name);
    bool unload_plugin(const std::string& name);

    // 工具调用
    ToolResult call_tool(const std::string& tool_name, const json& params);
    std::vector<std::string> list_tools() const;

    // 事件广播
    void broadcast_event(const Event& event);

    // 命令处理
    CommandResult execute_command(const CommandContext& ctx);

private:
    PluginManager() = default;
    std::map<std::string, std::shared_ptr<MaiBotPlugin>> _plugins;
    std::map<std::string, std::string> _tool_to_plugin;  // 工具名 -> 插件名映射
};
