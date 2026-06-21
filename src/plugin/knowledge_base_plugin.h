#pragma once

#include "src/plugin/plugin_base.h"
#include <map>
#include <memory>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 知识库配置
// ──────────────────────────────────────────────────────

class KnowledgeBaseConfig : public PluginConfigBase {
public:
    std::string knowledge_dir = "./data/knowledge";
    int max_knowledge_items = 1000;

    void from_json(const json& j) override;
    json to_json() const override;
};

// ──────────────────────────────────────────────────────
// 知识库项（简化版 - 仅存储元数据和内容，搜索由embedding系统负责）
// ──────────────────────────────────────────────────────

struct KnowledgeItem {
    std::string id;
    std::string title;
    std::string content;
    std::string category;
    int64_t created_at;
    int64_t updated_at;
    int access_count = 0;
};

// ──────────────────────────────────────────────────────
// 知识库管理插件（精简版）
// ──────────────────────────────────────────────────────

class KnowledgeBasePlugin : public MaiBotPlugin {
public:
    KnowledgeBasePlugin();
    ~KnowledgeBasePlugin() override;

    std::string get_name() const override { return "knowledge_base"; }
    std::string get_version() const override { return "1.0.0"; }
    std::string get_description() const override {
        return "知识库系统 - 存储和管理知识项（搜索由embedding系统提供）";
    }

    void on_load() override;
    void on_unload() override;

    std::vector<ToolDefinition> get_tools() const override;
    void on_event(const Event& event) override;

    std::shared_ptr<PluginConfigBase> get_config() override;
    void set_config(const json& config_json) override;

private:
    // 工具处理（仅保留 CRUD 操作，不包括搜索）
    ToolResult handle_add_knowledge(const json& params);
    ToolResult handle_get_knowledge(const json& params);
    ToolResult handle_list_knowledge(const json& params);
    ToolResult handle_update_knowledge(const json& params);
    ToolResult handle_delete_knowledge(const json& params);
    ToolResult handle_get_knowledge_stats(const json& params);

    // 内部方法
    bool load_knowledge_base();
    bool save_knowledge_base();
    std::string generate_id();

    std::shared_ptr<KnowledgeBaseConfig> _config;
    std::map<std::string, KnowledgeItem> _knowledge_base;
    bool _initialized;
};

std::shared_ptr<MaiBotPlugin> create_knowledge_base_plugin();
