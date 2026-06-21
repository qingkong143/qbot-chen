#include "src/plugin/long_term_memory_plugin.h"
#include "src/infra/logger.h"
#include <ctime>
#include <sstream>
#include <cmath>
#include <algorithm>

void LongTermMemoryConfig::from_json(const json& j) {
    if (j.contains("memory_dir")) {
        memory_dir = j["memory_dir"];
    }
    if (j.contains("max_memories_per_user")) {
        max_memories_per_user = j["max_memories_per_user"];
    }
    if (j.contains("memory_retention_days")) {
        memory_retention_days = j["memory_retention_days"];
    }
    if (j.contains("enable_memory_decay")) {
        enable_memory_decay = j["enable_memory_decay"];
    }
    if (j.contains("decay_factor")) {
        decay_factor = j["decay_factor"];
    }
}

json LongTermMemoryConfig::to_json() const {
    json j;
    j["memory_dir"] = memory_dir;
    j["max_memories_per_user"] = max_memories_per_user;
    j["memory_retention_days"] = memory_retention_days;
    j["enable_memory_decay"] = enable_memory_decay;
    j["decay_factor"] = decay_factor;
    return j;
}

// ──────────────────────────────────────────────────────
// LongTermMemoryPlugin 实现
// ──────────────────────────────────────────────────────

LongTermMemoryPlugin::LongTermMemoryPlugin()
    : _config(std::make_shared<LongTermMemoryConfig>()),
      _initialized(false) {
    Logger::get().info("[长期记忆]", "插件对象创建");
}

LongTermMemoryPlugin::~LongTermMemoryPlugin() {
    Logger::get().info("[长期记忆]", "插件对象销毁");
}

void LongTermMemoryPlugin::on_load() {
    try {
        Logger::get().info("[长期记忆]", "开始加载...");

        if (!load_memories()) {
            Logger::get().warn("[长期记忆]", "未找到已有记忆数据");
        }

        _initialized = true;
        int total = 0;
        for (const auto& [_, memories] : _user_memories) {
            total += memories.size();
        }
        Logger::get().info("[长期记忆]", "插件加载完成，用户数: " + std::to_string(_user_memories.size()) +
                          ", 总记忆数: " + std::to_string(total));

    } catch (const std::exception& e) {
        Logger::get().error("[长期记忆]", "加载失败: " + std::string(e.what()));
        throw;
    }
}

void LongTermMemoryPlugin::on_unload() {
    try {
        Logger::get().info("[长期记忆]", "开始卸载...");

        if (!save_memories()) {
            Logger::get().warn("[长期记忆]", "保存记忆数据失败");
        }

        _initialized = false;
        Logger::get().info("[长期记忆]", "插件卸载完成");

    } catch (const std::exception& e) {
        Logger::get().error("[长期记忆]", "卸载失败: " + std::string(e.what()));
    }
}

std::vector<ToolDefinition> LongTermMemoryPlugin::get_tools() const {
    std::vector<ToolDefinition> tools;

    tools.push_back(ToolDefinition{
        "save_memory",
        "保存用户记忆",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"content", ToolParamType::STRING, "记忆内容", true},
            ToolParameter{"importance", ToolParamType::FLOAT, "重要性 (0-1)", false, 0.5f},
            ToolParameter{"memory_type", ToolParamType::STRING, "类型 (fact/preference/interaction)", false, "fact"}
        },
        [this](const json& params) { return const_cast<LongTermMemoryPlugin*>(this)->handle_save_memory(params); }
    });

    tools.push_back(ToolDefinition{
        "recall_memory",
        "回忆用户相关记忆",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"top_k", ToolParamType::INTEGER, "返回数量", false, 5}
        },
        [this](const json& params) { return const_cast<LongTermMemoryPlugin*>(this)->handle_recall_memory(params); }
    });

    tools.push_back(ToolDefinition{
        "list_memories",
        "列出用户所有记忆",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true},
            ToolParameter{"limit", ToolParamType::INTEGER, "限制数量", false, 100}
        },
        [this](const json& params) { return const_cast<LongTermMemoryPlugin*>(this)->handle_list_memories(params); }
    });

    tools.push_back(ToolDefinition{
        "delete_memory",
        "删除用户记忆",
        {
            ToolParameter{"memory_id", ToolParamType::STRING, "记忆 ID", true},
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID", true}
        },
        [this](const json& params) { return const_cast<LongTermMemoryPlugin*>(this)->handle_delete_memory(params); }
    });

    tools.push_back(ToolDefinition{
        "forget_old_memories",
        "遗忘过期记忆",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID (可选，为空全部)", false}
        },
        [this](const json& params) { return const_cast<LongTermMemoryPlugin*>(this)->handle_forget_old_memories(params); }
    });

    tools.push_back(ToolDefinition{
        "memory_stats",
        "获取记忆统计",
        {
            ToolParameter{"user_id", ToolParamType::STRING, "用户 ID (可选)", false}
        },
        [this](const json& params) { return const_cast<LongTermMemoryPlugin*>(this)->handle_memory_stats(params); }
    });

    return tools;
}

void LongTermMemoryPlugin::on_event(const Event& event) {
    // 可以监听用户交互事件
}

std::shared_ptr<PluginConfigBase> LongTermMemoryPlugin::get_config() {
    return _config;
}

void LongTermMemoryPlugin::set_config(const json& config_json) {
    if (_config) {
        _config->from_json(config_json);
        Logger::get().info("[长期记忆]", "配置已更新");
    }
}

bool LongTermMemoryPlugin::load_memories() {
    Logger::get().debug("[长期记忆]", "加载记忆数据");
    return true;
}

bool LongTermMemoryPlugin::save_memories() {
    Logger::get().debug("[长期记忆]", "保存记忆数据");
    return true;
}

std::string LongTermMemoryPlugin::generate_id() {
    static int counter = 0;
    std::stringstream ss;
    ss << "mem_" << std::time(nullptr) << "_" << (counter++);
    return ss.str();
}

double LongTermMemoryPlugin::calculate_memory_decay(int64_t age_seconds) {
    if (!_config->enable_memory_decay) {
        return 1.0;
    }
    int days = age_seconds / (24 * 3600);
    return std::pow(_config->decay_factor, days);
}

// ──────────────────────────────────────────────────────
// 工具实现
// ──────────────────────────────────────────────────────

ToolResult LongTermMemoryPlugin::handle_save_memory(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "save_memory", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");
        std::string content = params.at("content");
        double importance = params.value("importance", 0.5);
        std::string memory_type = params.value("memory_type", std::string("fact"));

        auto& memories = _user_memories[user_id];
        if (memories.size() >= (size_t)_config->max_memories_per_user) {
            return ToolResult{false, "save_memory", "该用户记忆已满"};
        }

        UserMemory mem;
        mem.id = generate_id();
        mem.user_id = user_id;
        mem.content = content;
        mem.importance = std::min(1.0, std::max(0.0, importance));
        mem.memory_type = memory_type;
        mem.created_at = std::time(nullptr);
        mem.updated_at = mem.created_at;

        memories.push_back(mem);

        json response;
        response["memory_id"] = mem.id;
        response["status"] = "saved";

        return ToolResult{true, "save_memory", "记忆已保存", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "save_memory", std::string(e.what())};
    }
}

ToolResult LongTermMemoryPlugin::handle_recall_memory(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "recall_memory", "插件未初始化"};
        }

        std::string user_id = params.at("user_id");
        int top_k = params.value("top_k", 5);

        json response;
        response["user_id"] = user_id;
        response["memories"] = json::array();

        auto it = _user_memories.find(user_id);
        if (it == _user_memories.end()) {
            return ToolResult{true, "recall_memory", "该用户无记忆", {{"response", response}}};
        }

        // 按重要性和衰减排序
        std::vector<std::pair<double, UserMemory>> scored;
        int64_t now = std::time(nullptr);

        for (const auto& mem : it->second) {
            double decay = calculate_memory_decay(now - mem.created_at);
            double score = mem.importance * decay;
            scored.push_back({score, mem});
        }

        std::sort(scored.rbegin(), scored.rend(),
                 [](const auto& a, const auto& b) { return a.first < b.first; });

        int count = 0;
        for (const auto& [score, mem] : scored) {
            if (count >= top_k) break;

            json mem_item;
            mem_item["memory_id"] = mem.id;
            mem_item["content"] = mem.content;
            mem_item["importance"] = mem.importance;
            mem_item["relevance_score"] = score;
            mem_item["memory_type"] = mem.memory_type;

            response["memories"].push_back(mem_item);
            count++;
        }

        return ToolResult{true, "recall_memory", "记忆回忆完成", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "recall_memory", std::string(e.what())};
    }
}

ToolResult LongTermMemoryPlugin::handle_list_memories(const json& params) {
    try {
        std::string user_id = params.at("user_id");
        int limit = params.value("limit", 100);

        json response;
        response["user_id"] = user_id;
        response["memories"] = json::array();

        auto it = _user_memories.find(user_id);
        if (it != _user_memories.end()) {
            int count = 0;
            for (const auto& mem : it->second) {
                if (count >= limit) break;

                json mem_item;
                mem_item["memory_id"] = mem.id;
                mem_item["content"] = mem.content;
                mem_item["importance"] = mem.importance;
                mem_item["memory_type"] = mem.memory_type;

                response["memories"].push_back(mem_item);
                count++;
            }
        }

        response["total"] = it != _user_memories.end() ? (int)it->second.size() : 0;

        return ToolResult{true, "list_memories", "列表已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "list_memories", std::string(e.what())};
    }
}

ToolResult LongTermMemoryPlugin::handle_delete_memory(const json& params) {
    try {
        if (!_initialized) {
            return ToolResult{false, "delete_memory", "插件未初始化"};
        }

        std::string memory_id = params.at("memory_id");
        std::string user_id = params.at("user_id");

        auto it = _user_memories.find(user_id);
        if (it == _user_memories.end()) {
            return ToolResult{false, "delete_memory", "用户无记忆"};
        }

        auto& memories = it->second;
        auto mem_it = std::find_if(memories.begin(), memories.end(),
                                   [&](const UserMemory& m) { return m.id == memory_id; });

        if (mem_it == memories.end()) {
            return ToolResult{false, "delete_memory", "记忆不存在"};
        }

        memories.erase(mem_it);

        json response;
        response["memory_id"] = memory_id;
        response["status"] = "deleted";

        return ToolResult{true, "delete_memory", "记忆已删除", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "delete_memory", std::string(e.what())};
    }
}

ToolResult LongTermMemoryPlugin::handle_forget_old_memories(const json& params) {
    try {
        int retention_seconds = _config->memory_retention_days * 24 * 3600;
        int64_t now = std::time(nullptr);
        int deleted_count = 0;

        std::string target_user = "";
        if (params.contains("user_id")) {
            target_user = params["user_id"];
        }

        for (auto& [user_id, memories] : _user_memories) {
            if (!target_user.empty() && user_id != target_user) {
                continue;
            }

            auto it = memories.begin();
            while (it != memories.end()) {
                if ((now - it->created_at) > retention_seconds) {
                    it = memories.erase(it);
                    deleted_count++;
                } else {
                    ++it;
                }
            }
        }

        json response;
        response["deleted_count"] = deleted_count;
        response["retention_days"] = _config->memory_retention_days;

        return ToolResult{true, "forget_old_memories", "过期记忆已遗忘", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "forget_old_memories", std::string(e.what())};
    }
}

ToolResult LongTermMemoryPlugin::handle_memory_stats(const json& params) {
    try {
        json response;
        response["total_users"] = (int)_user_memories.size();
        response["total_memories"] = 0;

        if (params.contains("user_id")) {
            std::string user_id = params["user_id"];
            auto it = _user_memories.find(user_id);

            if (it != _user_memories.end()) {
                response["user_memory_count"] = (int)it->second.size();
                double avg_importance = 0;
                for (const auto& mem : it->second) {
                    avg_importance += mem.importance;
                }
                response["average_importance"] = it->second.size() > 0 ? avg_importance / it->second.size() : 0;
            } else {
                response["user_memory_count"] = 0;
                response["average_importance"] = 0;
            }
        } else {
            int total = 0;
            for (const auto& [_, memories] : _user_memories) {
                total += memories.size();
            }
            response["total_memories"] = total;
        }

        return ToolResult{true, "memory_stats", "统计已获取", {{"response", response}}};

    } catch (const std::exception& e) {
        return ToolResult{false, "memory_stats", std::string(e.what())};
    }
}

std::shared_ptr<MaiBotPlugin> create_long_term_memory_plugin() {
    return std::make_shared<LongTermMemoryPlugin>();
}


