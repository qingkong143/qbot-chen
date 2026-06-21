#include "src/plugin/knowledge_base_plugin.h"
#include "src/infra/logger.h"
#include <ctime>
#include <sstream>

void KnowledgeBaseConfig::from_json(const json& j) {
    if (j.contains("knowledge_dir")) {
        knowledge_dir = j["knowledge_dir"];
    }
    if (j.contains("max_knowledge_items")) {
        max_knowledge_items = j["max_knowledge_items"];
    }
}

json KnowledgeBaseConfig::to_json() const {
    json j;
    j["knowledge_dir"] = knowledge_dir;
    j["max_knowledge_items"] = max_knowledge_items;
    return j;
}

// ──────────────────────────────────────────────────────
// KnowledgeBasePlugin 实现
// ──────────────────────────────────────────────────────

KnowledgeBasePlugin::KnowledgeBasePlugin()
    : _config(std::make_shared<KnowledgeBaseConfig>()),
      _initialized(false) {
    Logger::get().info("[知识库]", "插件对象创建");
}

KnowledgeBasePlugin::~KnowledgeBasePlugin() {
    Logger::get().info("[知识库]", "插件对象销毁");
}

void KnowledgeBasePlugin::on_load() {
    try {
        Logger::get().info("[知识库]", "开始加载...");

        if (!load_knowledge_base()) {
            Logger::get().warn("[知识库]", "未找到已有知识库");
        }

        _initialized = true;
        Logger::get().info("[知识库]", "插件加载完成，知识项数: " + std::to_string(_knowledge_base.size()));

    } catch (const std::exception& e) {
        Logger::get().error("[知识库]", "加载失败: " + std::string(e.what()));
        throw;
    }
}

void KnowledgeBasePlugin::on_unload() {
    try {
        Logger::get().info("[知识库]", "开始卸载...");

        if (!save_knowledge_base()) {
            Logger::get().warn("[知识库]", "保存知识库失败");
        }

        _initialized = false;
        Logger::get().info("[知识库]", "插件卸载完成");

    } catch (const std::exception& e) {
        Logger::get().error("[知识库]", "卸载失败: " + std::string(e.what()));
    }
}

std::vector<ToolDefinition> KnowledgeBasePlugin::get_tools() const {
    std::vector<ToolDefinition> tools;

    tools.push_back(ToolDefinition{
        "add_knowledge",
        "添加知识项",
        {
            ToolParameter{"title", ToolParamType::STRING, "标题", true},
            ToolParameter{"content", ToolParamType::STRING, "内容", true},
            ToolParameter{"category", ToolParamType::STRING, "分类", false, "general"}
        },
        [this](const json& params) { return const_cast<KnowledgeBasePlugin*>(this)->handle_add_knowledge(params); }
    });

    tools.push_back(ToolDefinition{
        "get_knowledge",
        "获取知识项详情",
        {
            ToolParameter{"id", ToolParamType::STRING, "知识项 ID", true}
        },
        [this](const json& params) { return const_cast<KnowledgeBasePlugin*>(this)->handle_get_knowledge(params); }
    });

    tools.push_back(ToolDefinition{
        "list_knowledge",
        "列出所有知识项",
        {
            ToolParameter{"limit", ToolParamType::INTEGER, "限制数量", false, 100},
            ToolParameter{"category", ToolParamType::STRING, "按分类过滤 (可选)", false}
        },
        [this](const json& params) { return const_cast<KnowledgeBasePlugin*>(this)->handle_list_knowledge(params); }
    });

    tools.push_back(ToolDefinition{
        "update_knowledge",
        "更新知识项",
        {
            ToolParameter{"id", ToolParamType::STRING, "知识项 ID", true},
            ToolParameter{"title", ToolParamType::STRING, "新标题 (可选)", false},
            ToolParameter{"content", ToolParamType::STRING, "新内容 (可选)", false}
        },
        [this](const json& params) { return const_cast<KnowledgeBasePlugin*>(this)->handle_update_knowledge(params); }
    });

    tools.push_back(ToolDefinition{
        "delete_knowledge",
        "删除知识项",
        {
            ToolParameter{"id", ToolParamType::STRING, "知识项 ID", true}
        },
        [this](const json& params) { return const_cast<KnowledgeBasePlugin*>(this)->handle_delete_knowledge(params); }
    });

    tools.push_back(ToolDefinition{
        "get_knowledge_stats",
        "获取知识库统计",
        {},
        [this](const json& params) { return const_cast<KnowledgeBasePlugin*>(this)->handle_get_knowledge_stats(params); }
    });

    return tools;
}

void KnowledgeBasePlugin::on_event(const Event& event) {
    // 知识库可以监听embedding系统的事件
}

std::shared_ptr<PluginConfigBase> KnowledgeBasePlugin::get_config() {
    return _config;
}

void KnowledgeBasePlugin::set_config(const json& config_json) {
    if (_config) {
        _config->from_json(config_json);
        Logger::get().info("[知识库]", "配置已更新");
    }
}

bool KnowledgeBasePlugin::load_knowledge_base() {
    Logger::get().debug("[知识库]", "加载知识库");
    return true;
}

bool KnowledgeBasePlugin::save_knowledge_base() {
    Logger::get().debug("[知识库]", "保存知识库");
    return true;
}

std::string KnowledgeBasePlugin::generate_id() {
    static int counter = 0;
    std::stringstream ss;
    ss << "kb_" << std::time(nullptr) << "_" << (counter++);
    return ss.str();
}

// ──────────────────────────────────────────────────────
// 工具实现
// ──────────────────────────────────────────────────────

ToolResult KnowledgeBasePlugin::handle_add_knowledge(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "add_knowledge", "插件未初始化"};
        }

        std::string title = params.at("title");
        std::string content = params.at("content");
        std::string category = params.value("category", std::string("general"));

        if (_knowledge_base.size() >= (size_t)_config->max_knowledge_items) {
            return ToolResult{false, "add_knowledge", "知识库已满"};
        }

        KnowledgeItem item;
        item.id = generate_id();
        item.title = title;
        item.content = content;
        item.category = category;
        item.created_at = std::time(nullptr);
        item.updated_at = item.created_at;

        _knowledge_base[item.id] = item;

        json response;
        response["id"] = item.id;
        response["title"] = item.title;
        response["status"] = "added";

        return ToolResult{true, "add_knowledge", "知识项已添加", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "add_knowledge", std::string(e.what())};
    }
}

ToolResult KnowledgeBasePlugin::handle_get_knowledge(const json& params) {
    try {
        std::string id = params.at("id");

        auto it = _knowledge_base.find(id);
        if (it == _knowledge_base.end()) {
            return ToolResult{false, "get_knowledge", "知识项不存在: " + id};
        }

        auto& item = it->second;
        item.access_count++;

        json response;
        response["id"] = item.id;
        response["title"] = item.title;
        response["content"] = item.content;
        response["category"] = item.category;
        response["access_count"] = item.access_count;

        return ToolResult{true, "get_knowledge", "知识项已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "get_knowledge", std::string(e.what())};
    }
}

ToolResult KnowledgeBasePlugin::handle_list_knowledge(const json& params) {
    try {
        int limit = params.value("limit", 100);
        std::string filter_category = "";
        if (params.contains("category")) {
            filter_category = params["category"];
        }

        json response;
        response["items"] = json::array();
        response["total"] = (int)_knowledge_base.size();

        int count = 0;
        for (auto& [id, item] : _knowledge_base) {
            if (count >= limit) break;

            if (!filter_category.empty() && item.category != filter_category) {
                continue;
            }

            json res_item;
            res_item["id"] = id;
            res_item["title"] = item.title;
            res_item["category"] = item.category;
            res_item["access_count"] = item.access_count;

            response["items"].push_back(res_item);
            count++;
        }

        return ToolResult{true, "list_knowledge", "列表已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "list_knowledge", std::string(e.what())};
    }
}

ToolResult KnowledgeBasePlugin::handle_update_knowledge(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "update_knowledge", "插件未初始化"};
        }

        std::string id = params.at("id");

        auto it = _knowledge_base.find(id);
        if (it == _knowledge_base.end()) {
            return ToolResult{false, "update_knowledge", "知识项不存在: " + id};
        }

        if (params.contains("title")) {
            it->second.title = params["title"];
        }

        if (params.contains("content")) {
            it->second.content = params["content"];
        }

        it->second.updated_at = std::time(nullptr);

        json response;
        response["id"] = id;
        response["status"] = "updated";

        return ToolResult{true, "update_knowledge", "知识项已更新", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "update_knowledge", std::string(e.what())};
    }
}

ToolResult KnowledgeBasePlugin::handle_delete_knowledge(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "delete_knowledge", "插件未初始化"};
        }

        std::string id = params.at("id");

        auto it = _knowledge_base.find(id);
        if (it == _knowledge_base.end()) {
            return ToolResult{false, "delete_knowledge", "知识项不存在: " + id};
        }

        _knowledge_base.erase(it);

        json response;
        response["id"] = id;
        response["status"] = "deleted";

        return ToolResult{true, "delete_knowledge", "知识项已删除", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "delete_knowledge", std::string(e.what())};
    }
}

ToolResult KnowledgeBasePlugin::handle_get_knowledge_stats(const json& params) {
    try {
        int total_access = 0;
        std::map<std::string, int> category_count;

        for (const auto& [_, item] : _knowledge_base) {
            total_access += item.access_count;
            category_count[item.category]++;
        }

        json response;
        response["total_items"] = (int)_knowledge_base.size();
        response["total_access"] = total_access;
        response["categories"] = json::object();

        for (const auto& [cat, count] : category_count) {
            response["categories"][cat] = count;
        }

        return ToolResult{true, "get_knowledge_stats", "统计已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "get_knowledge_stats", std::string(e.what())};
    }
}

std::shared_ptr<MaiBotPlugin> create_knowledge_base_plugin() {
    return std::make_shared<KnowledgeBasePlugin>();
}


