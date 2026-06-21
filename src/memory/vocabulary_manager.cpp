#include "src/memory/vocabulary_manager.h"
#include "src/memory/style_learner.h"
#include "src/memory/jargon_miner.h"
#include "src/memory/style_cache.h"
#include "src/infra/logger.h"

VocabularyManager& VocabularyManager::get() {
    static VocabularyManager instance;
    return instance;
}

void VocabularyManager::initialize(const std::string& cache_path,
                                   const std::string& jargon_path) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_initialized) return;

    try {
        StyleCache::get().load(cache_path);
        JargonMiner::get().initialize(jargon_path);
        _initialized = true;
        Logger::get().info("[词汇管理]", "初始化完成");
    } catch (const std::exception& e) {
        Logger::get().error("[词汇管理]", "初始化失败: " + std::string(e.what()));
    }
}

void VocabularyManager::recordMessage(int64_t group_id,
                                     const std::string& user_id,
                                     const std::string& message,
                                     const std::vector<std::string>& recent) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    // 1. 记录消息到 StyleLearner（用于风格提取）
    StyleLearner::get().recordMessageStyle(group_id, message, user_id);

    // 2. 扫描消息收集行话
    JargonMiner::get().scanMessage(group_id, user_id, message, recent);

    // 3. 批量保存数据（每条消息处理完后）
    JargonMiner::get().flushToFile();
}

std::string VocabularyManager::getGroupContext(int64_t group_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return "";

    std::string context;

    // 1. 获取群风格（缓存过期时重新提取）
    std::string style = StyleCache::get().get(group_id);
    if (style.empty() || StyleCache::get().isExpired(group_id)) {
        // 从 StyleLearner 提取近期趋势并写入缓存
        std::string trend = StyleLearner::get().getRecentTrendInsights(group_id);
        if (!trend.empty()) {
            json style_data;
            style_data["keywords"] = trend;
            style_data["tone"] = "casual";
            StyleCache::get().set(group_id, style_data);
            StyleCache::get().save();
            style = StyleCache::get().get(group_id);
        }
    }
    if (!style.empty()) {
        context += "[群风格参考]\n" + style + "\n";
    }

    // 2. 获取推荐行话
    std::string jargon = JargonMiner::get().getRecommendedUsage(group_id);
    if (!jargon.empty()) {
        context += jargon;
    }

    return context;
}

void VocabularyManager::recordFeedback(int64_t group_id,
                                      const std::string& term,
                                      const std::string& feedback) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    JargonMiner::get().recordFeedback(group_id, term, feedback);
    JargonMiner::get().flushToFile();  // 批量保存
}

void VocabularyManager::inferJargonMeanings(int64_t group_id, int max_infer) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    // 获取群组所有词汇
    auto jargons = JargonMiner::get().getGroupJargons(group_id);

    int inferred = 0;
    for (const auto& jargon : jargons) {
        // 跳过已完成推理的词汇
        if (jargon.is_complete) {
            continue;
        }

        // 最多推理 max_infer 个词汇
        if (inferred >= max_infer) {
            break;
        }

        // 手动触发推理
        JargonMiner::get().triggerInference(group_id, jargon.content);
        inferred++;
    }
}

json VocabularyManager::getJargonData(int64_t group_id) const {
    auto jargons = JargonMiner::get().getGroupJargons(group_id);
    json result = json::array();
    for (const auto& j : jargons) {
        json item;
        item["content"] = j.content;
        item["meaning"] = j.meaning;
        item["count"] = j.count;
        item["is_jargon"] = j.is_jargon;
        item["is_complete"] = j.is_complete;
        result.push_back(item);
    }
    return result;
}

void VocabularyManager::clearGroup(int64_t group_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    StyleCache::get().invalidate(group_id);
    Logger::get().info("[词汇管理]", "清空群 " + std::to_string(group_id) + " 的缓存数据");
}

void VocabularyManager::shutdown() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    StyleCache::get().save();
    _initialized = false;
    Logger::get().info("[词汇管理]", "已关闭");
}

void VocabularyManager::setInferenceThresholds(const std::vector<int>& thresholds) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    JargonMiner::get().setInferenceThresholds(thresholds);
    Logger::get().info("[词汇管理]", "已更新推理频率阈值");
}

void VocabularyManager::applyTimeDecay(int64_t group_id, int days_threshold) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    JargonMiner::get().applyTimeDecay(group_id, days_threshold);
    JargonMiner::get().flushToFile();  // 批量保存
}

void VocabularyManager::setMaxRecommendations(int max_count) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized) return;

    JargonMiner::get().setMaxRecommendations(max_count);
}

int VocabularyManager::getMaxRecommendations() const {
    // 注意：这个方法是 const，所以不能加锁（为了避免 mutable mutex）
    // 在实际使用中应该在持有锁的上下文中调用
    return JargonMiner::get().getMaxRecommendations();
}
