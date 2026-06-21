#pragma once

#include "src/plugin/plugin_base.h"
#include <map>
#include <memory>
#include <set>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 用户管理配置
// ──────────────────────────────────────────────────────

class UserManagementConfig : public PluginConfigBase {
public:
    std::string user_data_dir = "./data/users";
    int max_users = 10000;
    bool enable_user_verification = true;

    void from_json(const json& j) override;
    json to_json() const override;
};

// ──────────────────────────────────────────────────────
// 用户信息
// ──────────────────────────────────────────────────────

struct UserInfo {
    std::string user_id;
    std::string nickname;
    std::string avatar;
    int64_t created_at;
    int64_t last_active;
    int interaction_count = 0;
    std::set<std::string> roles;  // admin, user, moderator
    std::map<std::string, std::string> metadata;  // 自定义数据
    bool is_active = true;
};

// ──────────────────────────────────────────────────────
// 用户管理插件
// ──────────────────────────────────────────────────────

class UserManagementPlugin : public MaiBotPlugin {
public:
    UserManagementPlugin();
    ~UserManagementPlugin() override;

    std::string get_name() const override { return "user_management"; }
    std::string get_version() const override { return "1.0.0"; }
    std::string get_description() const override {
        return "用户管理系统 - 创建、查询、更新用户信息";
    }

    void on_load() override;
    void on_unload() override;

    std::vector<ToolDefinition> get_tools() const override;
    void on_event(const Event& event) override;

    std::shared_ptr<PluginConfigBase> get_config() override;
    void set_config(const json& config_json) override;

private:
    // 工具处理
    ToolResult handle_create_user(const json& params);
    ToolResult handle_get_user(const json& params);
    ToolResult handle_update_user(const json& params);
    ToolResult handle_delete_user(const json& params);
    ToolResult handle_list_users(const json& params);
    ToolResult handle_add_role(const json& params);
    ToolResult handle_remove_role(const json& params);
    ToolResult handle_get_user_stats(const json& params);
    ToolResult handle_update_last_active(const json& params);

    // 内部方法
    bool load_users();
    bool save_users();
    bool user_exists(const std::string& user_id);

    std::shared_ptr<UserManagementConfig> _config;
    std::map<std::string, UserInfo> _users;
    bool _initialized;
};

std::shared_ptr<MaiBotPlugin> create_user_management_plugin();
