#pragma once

#include "src/plugin/plugin_base.h"
#include "src/knowledge/embedding_service.h"
#include "src/knowledge/embedding_store.h"
#include <memory>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 向量系统配置
// ──────────────────────────────────────────────────────

class EmbeddingSystemConfig : public PluginConfigBase {
public:
    // 缓存配置
    size_t cache_max_entries = 10000;
    size_t cache_max_size_mb = 100;
    int64_t cache_ttl_seconds = 86400;

    // 存储配置
    std::string store_dir = "./data/embedding";

    // embedding 服务配置（继承自全局 config）
    // api_url, api_key, model 由 Config::get().embedding() 提供

    void from_json(const json& j) override;
    json to_json() const override;
};

// ──────────────────────────────────────────────────────
// 向量系统插件
// ──────────────────────────────────────────────────────

class EmbeddingSystemPlugin : public MaiBotPlugin {
public:
    EmbeddingSystemPlugin();
    ~EmbeddingSystemPlugin() override;

    // 插件信息
    std::string get_name() const override { return "embedding_system"; }
    std::string get_version() const override { return "2.0.0"; }
    std::string get_description() const override {
        return "向量检索系统 - 提供 embedding 生成、向量存储和相似度搜索能力";
    }

    // 生命周期
    void on_load() override;
    void on_unload() override;

    // 工具接口
    std::vector<ToolDefinition> get_tools() const override;

    // 事件处理
    void on_event(const Event& event) override;

    // 配置
    std::shared_ptr<PluginConfigBase> get_config() override;
    void set_config(const json& config_json) override;

private:
    // 工具处理函数
    ToolResult handle_embed_text(const json& params);
    ToolResult handle_search(const json& params);
    ToolResult handle_add_document(const json& params);
    ToolResult handle_get_cache_stats(const json& params);
    ToolResult handle_check_model_consistency(const json& params);
    ToolResult handle_set_cache_config(const json& params);

    // 配置
    std::shared_ptr<EmbeddingSystemConfig> _config;

    // 状态标志
    bool _initialized;
    std::string _last_error;
};

// ──────────────────────────────────────────────────────
// 插件工厂函数
// ──────────────────────────────────────────────────────

std::shared_ptr<MaiBotPlugin> create_embedding_system_plugin();
