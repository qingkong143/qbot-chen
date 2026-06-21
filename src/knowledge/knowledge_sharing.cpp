#include "src/knowledge/knowledge_sharing.h"
#include "src/knowledge/embedding_store.h"
#include "src/infra/logger.h"

KnowledgeSharing& KnowledgeSharing::get() {
    static KnowledgeSharing instance;
    return instance;
}

void KnowledgeSharing::mark_as_global(const std::string& knowledge_hash) {
    std::lock_guard<std::mutex> lock(_mutex);
    _privacy_levels[knowledge_hash] = PrivacyLevel::PUBLIC;
    _stats.global_knowledge_count++;
    Logger::get().info("[知识共享] 标记为全局: ", knowledge_hash);
}

void KnowledgeSharing::mark_as_private(const std::string& knowledge_hash) {
    std::lock_guard<std::mutex> lock(_mutex);
    _privacy_levels[knowledge_hash] = PrivacyLevel::PRIVATE;
    _stats.private_knowledge_count++;
    Logger::get().info("[知识共享] 标记为私密: ", knowledge_hash);
}

bool KnowledgeSharing::can_share_to_group(const std::string& knowledge_hash, int64_t target_group_id) const {
    std::lock_guard<std::mutex> lock(_mutex);

    // 获取知识的隐私级别
    auto it = _privacy_levels.find(knowledge_hash);
    PrivacyLevel level = (it != _privacy_levels.end()) ? it->second : PrivacyLevel::GROUP;

    // 私密知识不共享
    if (level == PrivacyLevel::PRIVATE) {
        return false;
    }

    // 全局知识检查群组黑名单
    if (level == PrivacyLevel::PUBLIC) {
        auto settings_it = _group_settings.find(target_group_id);
        if (settings_it != _group_settings.end()) {
            const auto& settings = settings_it->second;
            if (settings.blocked_groups.count(target_group_id) > 0) {
                return false;
            }
            return settings.allow_import;
        }
        return true;  // 默认允许导入
    }

    // 群组级别知识仅限当前群
    return false;
}

std::vector<std::pair<std::string, float>> KnowledgeSharing::search_global_knowledge(
    const std::vector<float>& query, int k, int64_t exclude_group_id) {

    std::vector<std::pair<std::string, float>> results;

    // 从全局知识库搜索
    auto& manager = EmbeddingManager::get();

    // 这里应该有一个专门的全局知识库，暂时通过遍历实现
    // 实际应该由 EmbeddingManager 提供全局库支持

    Logger::get().debug("[知识共享] 全局搜索 (排除群: ", std::to_string(exclude_group_id) + ")");

    return results;
}

void KnowledgeSharing::set_group_settings(int64_t group_id, const GroupSharingSettings& settings) {
    std::lock_guard<std::mutex> lock(_mutex);
    _group_settings[group_id] = settings;
    Logger::get().info("[知识共享] 群 ", std::to_string(group_id) + " 设置已更新");
}

KnowledgeSharing::GroupSharingSettings KnowledgeSharing::get_group_settings(int64_t group_id) const {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _group_settings.find(group_id);
    if (it != _group_settings.end()) {
        return it->second;
    }

    // 返回默认设置
    return GroupSharingSettings();
}

void KnowledgeSharing::set_privacy_level(const std::string& knowledge_hash, PrivacyLevel level) {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string level_str;
    switch (level) {
        case PrivacyLevel::PUBLIC:
            level_str = "PUBLIC";
            break;
        case PrivacyLevel::GROUP:
            level_str = "GROUP";
            break;
        case PrivacyLevel::PRIVATE:
            level_str = "PRIVATE";
            break;
    }

    _privacy_levels[knowledge_hash] = level;
    Logger::get().debug("[知识共享] 设置隐私级别: ", knowledge_hash + " -> " + level_str);
}

KnowledgeSharing::PrivacyLevel KnowledgeSharing::get_privacy_level(const std::string& knowledge_hash) const {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _privacy_levels.find(knowledge_hash);
    if (it != _privacy_levels.end()) {
        return it->second;
    }

    return PrivacyLevel::GROUP;  // 默认群组级别
}

KnowledgeSharing::SharingStats KnowledgeSharing::get_stats() const {
    std::lock_guard<std::mutex> lock(_mutex);

    SharingStats stats = _stats;
    stats.global_knowledge_count = 0;
    stats.private_knowledge_count = 0;

    // 统计隐私级别分布
    for (const auto& [hash, level] : _privacy_levels) {
        if (level == PrivacyLevel::PUBLIC) {
            stats.global_knowledge_count++;
        } else if (level == PrivacyLevel::PRIVATE) {
            stats.private_knowledge_count++;
        }
    }

    return stats;
}
