#include "src/knowledge/knowledge_retriever.h"
#include "src/knowledge/embedding_service.h"
#include "src/knowledge/embedding_store.h"
#include "src/memory/jargon_miner.h"
#include "src/infra/logger.h"
#include <algorithm>
#include <regex>

KnowledgeRetriever& KnowledgeRetriever::get() {
    static KnowledgeRetriever instance;
    return instance;
}

bool KnowledgeRetriever::is_valid_message(const std::string& msg) {
    if (msg.empty() || msg.length() < 2) return false;

    // 过滤纯 CQ 码消息
    if (msg.find("[CQ:") != std::string::npos && msg.length() < 50) {
        return false;
    }

    // 过滤纯数字
    bool has_non_digit = false;
    for (char c : msg) {
        if ((c < '0' || c > '9') && c != ' ') {
            has_non_digit = true;
            break;
        }
    }
    return has_non_digit;
}

std::string KnowledgeRetriever::extract_content(const std::string& msg) {
    std::string content = msg;

    // 移除 CQ 码
    std::regex cq_pattern(R"(\[CQ:[^\]]*\])");
    content = std::regex_replace(content, cq_pattern, "");

    // 移除前后空白
    content.erase(0, content.find_first_not_of(" \t\n\r"));
    content.erase(content.find_last_not_of(" \t\n\r") + 1);

    return content;
}

std::vector<float> KnowledgeRetriever::get_message_embedding(const std::string& msg) {
    auto result = EmbeddingServiceClient::get().embed_text_sync(msg);
    if (result.success) {
        return result.embedding;
    }
    return {};
}

// ==================== 功能 1：群聊知识库检索 ====================

std::string KnowledgeRetriever::retrieve_knowledge(int64_t group_id, const std::string& question) {
    if (question.empty()) {
        return "";
    }

    Logger::get().debug("[知识检索] 开始检索群 ", std::to_string(group_id) + " 的知识库");

    // 获取问题的 embedding
    auto question_embedding = get_message_embedding(question);
    if (question_embedding.empty()) {
        Logger::get().warn("[知识检索] ", "无法获取问题的 embedding");
        return "";
    }

    // 搜索群组的知识库
    auto& manager = EmbeddingManager::get();
    std::string knowledge_store_name = "knowledge_group_" + std::to_string(group_id);

    // 动态获取或创建库
    auto& store = manager.get_store(knowledge_store_name);

    if (store.size() == 0) {
        Logger::get().debug("[知识检索] 群 ", std::to_string(group_id) + " 的知识库为空");
        return "";
    }

    // 搜索 top-5 相关知识
    auto results = store.search_top_k(question_embedding, 5);

    if (results.empty()) {
        Logger::get().debug("[知识检索] ", "未找到相关知识");
        return "";
    }

    // 构造知识注入提示
    std::string knowledge_context = "[相关群知识参考]\n";
    int count = 0;
    for (const auto& [hash, similarity] : results) {
        if (similarity < 0.5f) break;  // 相似度阈值

        std::string content = store.get_content(hash);
        if (!content.empty() && count < 3) {
            knowledge_context += "- " + content + " (相似度: " +
                                std::to_string((int)(similarity * 100)) + "%)\n";
            count++;
        }
    }

    if (count == 0) {
        return "";
    }

    Logger::get().info("[知识检索] 检索完成: 返回 ", std::to_string(count) + " 条相关知识");
    return knowledge_context;
}

// ==================== 功能 2：行话库学习 ====================

void KnowledgeRetriever::learn_jargon(int64_t group_id, const std::string& user_id,
                                     const std::string& message, const std::vector<std::string>& recent_messages) {
    if (!is_valid_message(message)) {
        return;
    }

    Logger::get().debug("[行话学习] 开始学习群 ", std::to_string(group_id) + " 的消息");

    // 方案：使用 jargon_miner 收集新词
    JargonMiner::get().scanMessage(group_id, user_id, message, recent_messages);

    // 同时将有价值的消息段落存入知识库
    std::string content = extract_content(message);

    if (content.length() > 10 && content.length() < 500) {
        std::string knowledge_store_name = "knowledge_group_" + std::to_string(group_id);
        auto& store = EmbeddingManager::get().get_store(knowledge_store_name);
        store.set_max_size(2000);  // 知识库上限 2000 条

        // batch_insert_strs 内部会调 embedding API 并做去重
        store.batch_insert_strs({content});

        Logger::get().debug("[行话学习] 已存储消息段落: ", content.substr(0, 50) + "...");
    }
}

// ==================== 功能 3：相似消息去重 ====================

bool KnowledgeRetriever::is_duplicate_message(int64_t group_id, const std::string& message,
                                             float similarity_threshold) {
    if (!is_valid_message(message)) {
        return false;
    }

    Logger::get().debug("[去重检测] 检查群 ", std::to_string(group_id) + " 的消息重复率");

    // 获取当前消息的 embedding
    auto message_embedding = get_message_embedding(message);
    if (message_embedding.empty()) {
        return false;
    }

    // 搜索历史库中最相似的消息
    std::string history_store_name = "history_group_" + std::to_string(group_id);
    auto& manager = EmbeddingManager::get();
    auto& store = manager.get_store(history_store_name);
    store.set_max_size(500);  // 历史库上限 500 条，超出淘汰最旧

    if (store.size() == 0) {
        // 第一次，存储此消息
        store.batch_insert_strs({message});
        return false;
    }

    // 搜索最相似的消息
    auto results = store.search_top_k(message_embedding, 1);

    if (!results.empty()) {
        float max_similarity = results[0].second;

        if (max_similarity >= similarity_threshold) {
            std::string similar_msg = store.get_content(results[0].first);
            Logger::get().warn("[去重检测] ", "检测到重复消息，相似度: " +
                             std::to_string((int)(max_similarity * 100)) + "%, 历史: " +
                             similar_msg.substr(0, 50));
            return true;
        }
    }

    // 不是重复，存储此消息以供后续对比
    store.batch_insert_strs({message});

    return false;
}

void KnowledgeRetriever::save_all() {
    EmbeddingManager::get().save_all();
    Logger::get().info("[知识检索] ", "所有库已保存");
}

void KnowledgeRetriever::clear_group_knowledge(int64_t group_id) {
    auto& manager = EmbeddingManager::get();

    std::string knowledge_store_name = "knowledge_group_" + std::to_string(group_id);
    std::string history_store_name = "history_group_" + std::to_string(group_id);

    manager.get_store(knowledge_store_name).clear();
    manager.get_store(history_store_name).clear();

    Logger::get().info("[知识检索] 已清空群 ", std::to_string(group_id) + " 的知识库");
}
