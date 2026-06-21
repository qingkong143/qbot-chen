#include "src/plugin/embedding_plugin.h"
#include "src/infra/logger.h"
#include "src/core/config.h"

// ──────────────────────────────────────────────────────
// EmbeddingSystemConfig 实现
// ──────────────────────────────────────────────────────

void EmbeddingSystemConfig::from_json(const json& j) {
    if (j.contains("cache_max_entries")) {
        cache_max_entries = j["cache_max_entries"];
    }
    if (j.contains("cache_max_size_mb")) {
        cache_max_size_mb = j["cache_max_size_mb"];
    }
    if (j.contains("cache_ttl_seconds")) {
        cache_ttl_seconds = j["cache_ttl_seconds"];
    }
    if (j.contains("store_dir")) {
        store_dir = j["store_dir"];
    }
}

json EmbeddingSystemConfig::to_json() const {
    json j;
    j["cache_max_entries"] = cache_max_entries;
    j["cache_max_size_mb"] = cache_max_size_mb;
    j["cache_ttl_seconds"] = cache_ttl_seconds;
    j["store_dir"] = store_dir;
    return j;
}

// ──────────────────────────────────────────────────────
// EmbeddingSystemPlugin 实现
// ──────────────────────────────────────────────────────

EmbeddingSystemPlugin::EmbeddingSystemPlugin()
    : _config(std::make_shared<EmbeddingSystemConfig>()),
      _initialized(false) {
    Logger::get().info("[向量插件]", "插件对象创建");
}

EmbeddingSystemPlugin::~EmbeddingSystemPlugin() {
    Logger::get().info("[向量插件]", "插件对象销毁");
}

void EmbeddingSystemPlugin::on_load() {
    try {
        Logger::get().info("[向量插件]", "开始加载...");

        // 1. 初始化 embedding 服务
        auto& client = EmbeddingServiceClient::get();
        const auto& emb_config = Config::get().embedding();

        client.initialize(
            emb_config.api_url,
            emb_config.api_key,
            emb_config.model
        );
        Logger::get().info("[向量插件]", "Embedding 服务初始化完成");

        // 2. 配置缓存
        client.set_cache_max_entries(_config->cache_max_entries);
        client.set_cache_max_size_mb(_config->cache_max_size_mb);
        Logger::get().info("[向量插件]", "缓存配置完成"
                          " (max_entries=" + std::to_string(_config->cache_max_entries)
                          + ", max_size_mb=" + std::to_string(_config->cache_max_size_mb) + ")");

        // 3. 初始化向量存储
        auto& manager = EmbeddingManager::get();
        manager.initialize(_config->store_dir);
        Logger::get().info("[向量插件]", "向量存储初始化完成");

        // 4. 检查模型一致性
        if (!manager.check_all_embedding_model_consistency()) {
            Logger::get().warn("[向量插件]", "模型一致性校验失败！");
            Logger::get().warn("[向量插件]", "建议清空 " + _config->store_dir + " 后重试");
            _last_error = "Model consistency check failed";
        } else {
            Logger::get().info("[向量插件]", "模型一致性校验通过");
        }

        _initialized = true;
        Logger::get().info("[向量插件]", "插件加载完成");

    } catch (const std::exception& e) {
        _last_error = e.what();
        Logger::get().error("[向量插件]", "加载失败: " + std::string(e.what()));
        throw;
    }
}

void EmbeddingSystemPlugin::on_unload() {
    try {
        Logger::get().info("[向量插件]", "开始卸载...");

        // 打印最终的缓存统计
        EmbeddingServiceClient::get().print_cache_stats();

        _initialized = false;
        Logger::get().info("[向量插件]", "插件卸载完成");

    } catch (const std::exception& e) {
        Logger::get().error("[向量插件]", "卸载失败: " + std::string(e.what()));
    }
}

std::vector<ToolDefinition> EmbeddingSystemPlugin::get_tools() const {
    std::vector<ToolDefinition> tools;

    // 工具1：文本 embedding
    tools.push_back(ToolDefinition{
        "embed_text",
        "生成文本的向量表示 (embedding)",
        {
            ToolParameter{"text", ToolParamType::STRING, "要转换的文本", true},
            ToolParameter{"cache", ToolParamType::BOOLEAN, "是否使用缓存", false, true}
        },
        [this](const json& params) { return const_cast<EmbeddingSystemPlugin*>(this)->handle_embed_text(params); }
    });

    // 工具2：向量搜索
    tools.push_back(ToolDefinition{
        "search_similar",
        "在向量库中搜索相似内容",
        {
            ToolParameter{"query", ToolParamType::STRING, "查询文本", true},
            ToolParameter{"store_name", ToolParamType::STRING, "向量库名称 (如: knowledge, jargon)", true},
            ToolParameter{"top_k", ToolParamType::INTEGER, "返回最相似的 K 条", false, 5},
            ToolParameter{"min_similarity", ToolParamType::FLOAT, "最小相似度阈值 (0-1)", false, 0.5f}
        },
        [this](const json& params) { return const_cast<EmbeddingSystemPlugin*>(this)->handle_search(params); }
    });

    // 工具3：添加文档
    tools.push_back(ToolDefinition{
        "add_document",
        "添加文档到向量库",
        {
            ToolParameter{"content", ToolParamType::STRING, "文档内容", true},
            ToolParameter{"store_name", ToolParamType::STRING, "向量库名称", true},
            ToolParameter{"metadata", ToolParamType::OBJECT, "文档元数据 (可选)", false}
        },
        [this](const json& params) { return const_cast<EmbeddingSystemPlugin*>(this)->handle_add_document(params); }
    });

    // 工具4：缓存统计
    tools.push_back(ToolDefinition{
        "get_cache_stats",
        "获取缓存统计信息",
        {},
        [this](const json& params) { return const_cast<EmbeddingSystemPlugin*>(this)->handle_get_cache_stats(params); }
    });

    // 工具5：模型一致性检查
    tools.push_back(ToolDefinition{
        "check_model_consistency",
        "检查所有向量库的模型一致性",
        {},
        [this](const json& params) { return const_cast<EmbeddingSystemPlugin*>(this)->handle_check_model_consistency(params); }
    });

    // 工具6：设置缓存配置
    tools.push_back(ToolDefinition{
        "set_cache_config",
        "动态调整缓存配置",
        {
            ToolParameter{"max_entries", ToolParamType::INTEGER, "最大条目数", false},
            ToolParameter{"max_size_mb", ToolParamType::INTEGER, "最大大小 (MB)", false}
        },
        [this](const json& params) { return const_cast<EmbeddingSystemPlugin*>(this)->handle_set_cache_config(params); }
    });

    return tools;
}

void EmbeddingSystemPlugin::on_event(const Event& event) {
    // 可以在这里处理全局事件
    // 例如：PLUGIN_LOADED 时进行初始化，USER_JOINED 时预加载知识库等
}

std::shared_ptr<PluginConfigBase> EmbeddingSystemPlugin::get_config() {
    return _config;
}

void EmbeddingSystemPlugin::set_config(const json& config_json) {
    if (_config) {
        _config->from_json(config_json);
        Logger::get().info("[向量插件]", "配置已更新");
    }
}

// ──────────────────────────────────────────────────────
// 工具处理实现
// ──────────────────────────────────────────────────────

ToolResult EmbeddingSystemPlugin::handle_embed_text(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "embed_text", "插件未初始化"};
        }

        std::string text = params.at("text");
        bool use_cache = params.value("cache", true);

        auto result = EmbeddingServiceClient::get().embed_text_sync(text);

        if (!result.success) {
            return ToolResult{false, "embed_text", result.error_msg};
        }

        json response;
        response["embedding_dimension"] = result.embedding.size();
        response["embedding_sample"] = json::array();
        // 返回前 5 个维度作为样本
        for (size_t i = 0; i < std::min(size_t(5), result.embedding.size()); ++i) {
            response["embedding_sample"].push_back(result.embedding[i]);
        }
        response["cached"] = use_cache;

        return ToolResult{true, "embed_text", "embedding 生成成功", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "embed_text", std::string(e.what())};
    }
}

ToolResult EmbeddingSystemPlugin::handle_search(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "search_similar", "插件未初始化"};
        }

        std::string query = params.at("query");
        std::string store_name = params.at("store_name");
        int top_k = params.value("top_k", 5);
        float min_similarity = params.value("min_similarity", 0.5f);

        // 获取查询的 embedding
        auto emb_result = EmbeddingServiceClient::get().embed_text_sync(query);
        if (!emb_result.success) {
            return ToolResult{false, "search_similar", "无法获取查询 embedding: " + emb_result.error_msg};
        }

        // 搜索
        auto& manager = EmbeddingManager::get();
        auto& store = manager.get_store(store_name);

        auto results = store.search_top_k(emb_result.embedding, top_k);

        json response;
        response["query"] = query;
        response["store"] = store_name;
        response["results"] = json::array();

        for (const auto& [hash, similarity] : results) {
            if (similarity < min_similarity) break;

            json item;
            item["similarity"] = similarity;
            item["similarity_percent"] = (int)(similarity * 100);
            // content 从存储中获取
            item["content"] = store.get_content(hash);
            response["results"].push_back(item);
        }

        return ToolResult{true, "search_similar", "搜索完成", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "search_similar", std::string(e.what())};
    }
}

ToolResult EmbeddingSystemPlugin::handle_add_document(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "add_document", "插件未初始化"};
        }

        std::string content = params.at("content");
        std::string store_name = params.at("store_name");

        auto& manager = EmbeddingManager::get();
        auto& store = manager.get_store(store_name);

        // 获取文档的 embedding
        auto emb_result = EmbeddingServiceClient::get().embed_text_sync(content);
        if (!emb_result.success) {
            return ToolResult{false, "add_document", "无法获取 embedding: " + emb_result.error_msg};
        }

        // 添加到存储（使用batch_insert_strs）
        store.batch_insert_strs({content});

        json response;
        response["store"] = store_name;
        response["content_length"] = content.length();
        response["status"] = "added";

        return ToolResult{true, "add_document", "文档已添加", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "add_document", std::string(e.what())};
    }
}

ToolResult EmbeddingSystemPlugin::handle_get_cache_stats(const json& params) {
    try {
        auto& client = EmbeddingServiceClient::get();

        json response;
        response["cache_entries"] = (int)client.get_cache_entries_count();
        response["cache_size_mb"] = (int)client.get_cache_size();
        response["cache_max_entries"] = (int)_config->cache_max_entries;
        response["cache_max_size_mb"] = (int)_config->cache_max_size_mb;

        // 计算使用率
        float entry_usage = (float)client.get_cache_entries_count() / _config->cache_max_entries * 100;
        float size_usage = (float)client.get_cache_size() / (_config->cache_max_size_mb * 1024 * 1024) * 100;

        response["entry_usage_percent"] = (int)entry_usage;
        response["size_usage_percent"] = (int)size_usage;

        return ToolResult{true, "get_cache_stats", "缓存统计获取成功", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "get_cache_stats", std::string(e.what())};
    }
}

ToolResult EmbeddingSystemPlugin::handle_check_model_consistency(const json& params) {
    try {
        auto& manager = EmbeddingManager::get();
        bool consistent = manager.check_all_embedding_model_consistency();

        json response;
        response["all_consistent"] = consistent;
        response["status"] = consistent ? "通过" : "失败";

        return ToolResult{true, "check_model_consistency", "模型一致性检查完成", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "check_model_consistency", std::string(e.what())};
    }
}

ToolResult EmbeddingSystemPlugin::handle_set_cache_config(const json& params) {
    try {
        auto& client = EmbeddingServiceClient::get();

        if (params.contains("max_entries")) {
            size_t max_entries = params["max_entries"];
            client.set_cache_max_entries(max_entries);
            _config->cache_max_entries = max_entries;
            Logger::get().info("[向量插件]", "缓存最大条目数已设置: " + std::to_string(max_entries));
        }

        if (params.contains("max_size_mb")) {
            size_t max_size_mb = params["max_size_mb"];
            client.set_cache_max_size_mb(max_size_mb);
            _config->cache_max_size_mb = max_size_mb;
            Logger::get().info("[向量插件]", "缓存最大大小已设置: " + std::to_string(max_size_mb) + " MB");
        }

        json response;
        response["cache_max_entries"] = (int)_config->cache_max_entries;
        response["cache_max_size_mb"] = (int)_config->cache_max_size_mb;

        return ToolResult{true, "set_cache_config", "缓存配置已更新", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "set_cache_config", std::string(e.what())};
    }
}

// ──────────────────────────────────────────────────────
// 插件工厂函数
// ──────────────────────────────────────────────────────

std::shared_ptr<MaiBotPlugin> create_embedding_system_plugin() {
    return std::make_shared<EmbeddingSystemPlugin>();
}


