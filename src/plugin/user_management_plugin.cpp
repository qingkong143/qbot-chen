#include "src/plugin/user_management_plugin.h"
#include "src/infra/logger.h"
#include <ctime>
#include <algorithm>

void UserManagementConfig::from_json(const json& j) {
    if (j.contains("user_data_dir")) {
        user_data_dir = j["user_data_dir"];
    }
    if (j.contains("max_users")) {
        max_users = j["max_users"];
    }
    if (j.contains("enable_user_verification")) {
        enable_user_verification = j["enable_user_verification"];
    }
}

json UserManagementConfig::to_json() const {
    json j;
    j["user_data_dir"] = user_data_dir;
    j["max_users"] = max_users;
    j["enable_user_verification"] = enable_user_verification;
    return j;
}

// ──────────────────────────────────────────────────────
// UserManagementPlugin 实现
// ──────────────────────────────────────────────────────

UserManagementPlugin::UserManagementPlugin()
    : _config(std::make_shared<UserManagementConfig>()),
      _initialized(false) {
    Logger::get().info("[用户管理]", "插件对象创建");
}

UserManagementPlugin::~UserManagementPlugin() {
    Logger::get().info("[用户管理]", "插件对象销毁");
}

void UserManagementPlugin::on_load() {
    try {
        Logger::get().info("[用户管理]", "开始加载...");

        if (!load_users()) {
            Logger::get().warn("[用户管理]", "未找到已有用户数据");
        }

        _initialized = true;
        Logger::get().info("[用户管理]", "插件加载完成，用户数: " + std::to_string(_users.size()));

    } catch (const std::exception& e) {
        Logger::get().error("[用户管理]", "加载失败: " + std::string(e.what()));
        throw;
    }
}

void UserManagementPlugin::on_unload() {
    try {
        Logger::get().info("[用户管理]", "开始卸载...");

        if (!save_users()) {
            Logger::get().warn("[用户管理]", "保存用户数据失败");
        }

        _initialized = false;
        Logger::get().info("[用户管理]", "插件卸载完成");

    } catch (const std::exception& e) {
        Logger::get().error("[用户管理]", "卸载失败: " + std::string(e.what()));
    }
}

std::vector<ToolDefinition> UserManagementPlugin::get_tools() const {
    std::vector<ToolDefinition> tools;

    tools.push_back(ToolDefinition{
        "create_user",
        "创建新用户",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"nickname", ToolParamType::STRING, "昵称", true},
            ToolParameter{"avatar", ToolParamType::STRING, "头像 URL (可选)", false}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_create_user(params); }
    });

    tools.push_back(ToolDefinition{
        "get_user",
        "获取用户信息",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_get_user(params); }
    });

    tools.push_back(ToolDefinition{
        "update_user",
        "更新用户信息",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"nickname", ToolParamType::STRING, "新昵称 (可选)", false},
            ToolParameter{"avatar", ToolParamType::STRING, "新头像 URL (可选)", false}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_update_user(params); }
    });

    tools.push_back(ToolDefinition{
        "delete_user",
        "删除用户",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_delete_user(params); }
    });

    tools.push_back(ToolDefinition{
        "list_users",
        "列出所有用户",
        {
            ToolParameter{"limit", ToolParamType::INTEGER, "限制数量", false, 100},
            ToolParameter{"role", ToolParamType::STRING, "过滤角色 (可选)", false}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_list_users(params); }
    });

    tools.push_back(ToolDefinition{
        "add_role",
        "为用户添加角色",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"role", ToolParamType::STRING, "角色 (admin/user/moderator)", true}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_add_role(params); }
    });

    tools.push_back(ToolDefinition{
        "remove_role",
        "为用户移除角色",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"role", ToolParamType::STRING, "角色", true}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_remove_role(params); }
    });

    tools.push_back(ToolDefinition{
        "get_user_stats",
        "获取用户统计信息",
        {},
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_get_user_stats(params); }
    });

    tools.push_back(ToolDefinition{
        "update_last_active",
        "更新用户最后活跃时间",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true}
        },
        [this](const json& params) { return const_cast<UserManagementPlugin*>(this)->handle_update_last_active(params); }
    });

    return tools;
}

void UserManagementPlugin::on_event(const Event& event) {
    // 可以在这里响应系统事件
}

std::shared_ptr<PluginConfigBase> UserManagementPlugin::get_config() {
    return _config;
}

void UserManagementPlugin::set_config(const json& config_json) {
    if (_config) {
        _config->from_json(config_json);
        Logger::get().info("[用户管理]", "配置已更新");
    }
}

bool UserManagementPlugin::load_users() {
    Logger::get().debug("[用户管理]", "加载用户数据");
    return true;
}

bool UserManagementPlugin::save_users() {
    Logger::get().debug("[用户管理]", "保存用户数据");
    return true;
}

bool UserManagementPlugin::user_exists(const std::string& user_id) {
    return _users.find(user_id) != _users.end();
}

// ──────────────────────────────────────────────────────
// 工具实现
// ──────────────────────────────────────────────────────

ToolResult UserManagementPlugin::handle_create_user(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "create_user", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");
        std::string nickname = params.at("nickname");

        if (user_exists(user_id)) {
            return ToolResult{false, "create_user", "用户已存在: " + user_id};
        }

        if (_users.size() >= (size_t)_config->max_users) {
            return ToolResult{false, "create_user", "用户数已达上限"};
        }

        UserInfo user;
        user.user_id = user_id;
        user.nickname = nickname;
        user.avatar = params.value("avatar", std::string(""));
        user.created_at = std::time(nullptr);
        user.last_active = user.created_at;
        user.roles.insert("user");  // 默认角色

        _users[user_id] = user;

        json response;
        response["user_id"] = user_id;
        response["status"] = "created";

        return ToolResult{true, "create_user", "用户已创建", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "create_user", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_get_user(const json& params) {
    try {
        std::string user_id = params.at("user_id");

        auto it = _users.find(user_id);
        if (it == _users.end()) {
            return ToolResult{false, "get_user", "用户不存在: " + user_id};
        }

        const auto& user = it->second;

        json response;
        response["user_id"] = user.user_id;
        response["nickname"] = user.nickname;
        response["avatar"] = user.avatar;
        response["interaction_count"] = user.interaction_count;
        response["is_active"] = user.is_active;
        response["roles"] = json::array();

        for (const auto& role : user.roles) {
            response["roles"].push_back(role);
        }

        return ToolResult{true, "get_user", "用户信息已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "get_user", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_update_user(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "update_user", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");

        auto it = _users.find(user_id);
        if (it == _users.end()) {
            return ToolResult{false, "update_user", "用户不存在: " + user_id};
        }

        if (params.contains("nickname")) {
            it->second.nickname = params["nickname"];
        }

        if (params.contains("avatar")) {
            it->second.avatar = params["avatar"];
        }

        it->second.last_active = std::time(nullptr);

        json response;
        response["user_id"] = user_id;
        response["status"] = "updated";

        return ToolResult{true, "update_user", "用户已更新", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "update_user", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_delete_user(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "delete_user", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");

        auto it = _users.find(user_id);
        if (it == _users.end()) {
            return ToolResult{false, "delete_user", "用户不存在: " + user_id};
        }

        _users.erase(it);

        json response;
        response["user_id"] = user_id;
        response["status"] = "deleted";

        return ToolResult{true, "delete_user", "用户已删除", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "delete_user", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_list_users(const json& params) {
    try {
        int limit = params.value("limit", 100);

        json response;
        response["users"] = json::array();
        response["total"] = (int)_users.size();

        std::string filter_role = "";
        if (params.contains("role")) {
            filter_role = params["role"];
        }

        int count = 0;
        for (const auto& [user_id, user] : _users) {
            if (count >= limit) break;

            if (!filter_role.empty() && user.roles.find(filter_role) == user.roles.end()) {
                continue;
            }

            json user_item;
            user_item["user_id"] = user.user_id;
            user_item["nickname"] = user.nickname;
            user_item["interaction_count"] = user.interaction_count;
            user_item["roles"] = json::array();

            for (const auto& role : user.roles) {
                user_item["roles"].push_back(role);
            }

            response["users"].push_back(user_item);
            count++;
        }

        return ToolResult{true, "list_users", "用户列表已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "list_users", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_add_role(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "add_role", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");
        std::string role = params.at("role");

        auto it = _users.find(user_id);
        if (it == _users.end()) {
            return ToolResult{false, "add_role", "用户不存在: " + user_id};
        }

        it->second.roles.insert(role);

        json response;
        response["user_id"] = user_id;
        response["role"] = role;
        response["status"] = "added";

        return ToolResult{true, "add_role", "角色已添加", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "add_role", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_remove_role(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "remove_role", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");
        std::string role = params.at("role");

        auto it = _users.find(user_id);
        if (it == _users.end()) {
            return ToolResult{false, "remove_role", "用户不存在: " + user_id};
        }

        it->second.roles.erase(role);

        json response;
        response["user_id"] = user_id;
        response["role"] = role;
        response["status"] = "removed";

        return ToolResult{true, "remove_role", "角色已移除", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "remove_role", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_get_user_stats(const json& params) {
    try {
        int total_users = _users.size();
        int active_users = 0;
        int admin_count = 0;
        int total_interactions = 0;

        for (const auto& [_, user] : _users) {
            if (user.is_active) {
                active_users++;
            }
            if (user.roles.find("admin") != user.roles.end()) {
                admin_count++;
            }
            total_interactions += user.interaction_count;
        }

        json response;
        response["total_users"] = total_users;
        response["active_users"] = active_users;
        response["admin_count"] = admin_count;
        response["total_interactions"] = total_interactions;
        response["max_users"] = _config->max_users;
        response["usage_percent"] = total_users > 0 ? (int)(total_users * 100.0 / _config->max_users) : 0;

        return ToolResult{true, "get_user_stats", "统计信息已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "get_user_stats", std::string(e.what())};
    }
}

ToolResult UserManagementPlugin::handle_update_last_active(const json& params) {
    try {
        std::string user_id = params.at("user_id");

        auto it = _users.find(user_id);
        if (it == _users.end()) {
            return ToolResult{false, "update_last_active", "用户不存在: " + user_id};
        }

        it->second.last_active = std::time(nullptr);
        it->second.interaction_count++;

        json response;
        response["user_id"] = user_id;
        response["last_active"] = (long long)it->second.last_active;
        response["interaction_count"] = it->second.interaction_count;

        return ToolResult{true, "update_last_active", "用户活跃时间已更新", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "update_last_active", std::string(e.what())};
    }
}

std::shared_ptr<MaiBotPlugin> create_user_management_plugin() {
    return std::make_shared<UserManagementPlugin>();
}


