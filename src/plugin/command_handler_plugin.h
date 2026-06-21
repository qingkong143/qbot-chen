#pragma once

#include "src/plugin/plugin_base.h"
#include <map>
#include <memory>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 命令处理器配置
// ──────────────────────────────────────────────────────

class CommandHandlerConfig : public PluginConfigBase {
public:
    std::string command_prefix = "/";
    bool case_insensitive = true;

    void from_json(const json& j) override;
    json to_json() const override;
};

// ──────────────────────────────────────────────────────
// 命令处理插件（精简版 - 只提供命令路由，具体逻辑由其他插件实现）
// ──────────────────────────────────────────────────────

class CommandHandlerPlugin : public MaiBotPlugin {
public:
    CommandHandlerPlugin();
    ~CommandHandlerPlugin() override;

    std::string get_name() const override { return "command_handler"; }
    std::string get_version() const override { return "1.0.0"; }
    std::string get_description() const override {
        return "命令处理系统 - 提供系统信息和插件列表查询";
    }

    void on_load() override;
    void on_unload() override;

    std::vector<ToolDefinition> get_tools() const override;
    void on_event(const Event& event) override;

    std::shared_ptr<PluginConfigBase> get_config() override;
    void set_config(const json& config_json) override;

private:
    // 工具处理（简化为只提供信息查询）
    ToolResult handle_system_info(const json& params);
    ToolResult handle_plugin_list(const json& params);
    ToolResult handle_list_all_tools(const json& params);

    std::shared_ptr<CommandHandlerConfig> _config;
    bool _initialized;
};

std::shared_ptr<MaiBotPlugin> create_command_handler_plugin();
