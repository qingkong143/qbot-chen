#pragma once

#include "src/plugin/plugin_base.h"
#include <map>
#include <memory>
#include <deque>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 长期记忆配置
// ──────────────────────────────────────────────────────

class LongTermMemoryConfig : public PluginConfigBase {
public:
    std::string memory_dir = "./data/memory";
    int max_memories_per_user = 500;
    int memory_retention_days = 365;
    bool enable_memory_decay = true;
    double decay_factor = 0.95;

    void from_json(const json& j) override;
    json to_json() const override;
};

// ──────────────────────────────────────────────────────
// 用户记忆类型（精简版 - 只关注记忆，用户由 user_management 管理）
// ──────────────────────────────────────────────────────

struct UserMemory {
    std::string id;
    std::string user_id;
    std::string content;
    double importance = 0.5;
    int64_t created_at;
    int64_t updated_at;
    std::string memory_type;  // "fact", "preference", "interaction"
};

// ──────────────────────────────────────────────────────
// 长期记忆插件（精简版 - 只关注记忆存储和回忆）
// ──────────────────────────────────────────────────────

class LongTermMemoryPlugin : public MaiBotPlugin {
public:
    LongTermMemoryPlugin();
    ~LongTermMemoryPlugin() override;

    std::string get_name() const override { return "long_term_memory"; }
    std::string get_version() const override { return "1.0.0"; }
    std::string get_description() const override {
        return "长期记忆系统 - 记录和回忆用户交互信息";
    }

    void on_load() override;
    void on_unload() override;

    std::vector<ToolDefinition> get_tools() const override;
    void on_event(const Event& event) override;

    std::shared_ptr<PluginConfigBase> get_config() override;
    void set_config(const json& config_json) override;

private:
    // 工具处理
    ToolResult handle_save_memory(const json& params);
    ToolResult handle_recall_memory(const json& params);
    ToolResult handle_list_memories(const json& params);
    ToolResult handle_delete_memory(const json& params);
    ToolResult handle_forget_old_memories(const json& params);
    ToolResult handle_memory_stats(const json& params);

    // 内部方法
    bool load_memories();
    bool save_memories();
    std::string generate_id();
    double calculate_memory_decay(int64_t age_seconds);

    std::shared_ptr<LongTermMemoryConfig> _config;
    std::map<std::string, std::deque<UserMemory>> _user_memories;
    bool _initialized;
};

std::shared_ptr<MaiBotPlugin> create_long_term_memory_plugin();
