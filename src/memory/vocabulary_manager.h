#pragma once
#include "src/core/base.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>

using json = nlohmann::json;

// 词汇管理器 - 统一协调 StyleLearner, JargonMiner, StyleCache
// 职责：
//   1. 消息记录 -> StyleLearner
//   2. 行话挖掘 -> JargonMiner
//   3. 缓存管理 -> StyleCache
//   4. 统一对外接口

class VocabularyManager {
public:
    static VocabularyManager& get();

    // 初始化（加载缓存和行话数据）
    void initialize(const std::string& cache_path = "group_style_cache.json",
                    const std::string& jargon_path = "jargons.json");

    // 记录消息（内部调用 StyleLearner + JargonMiner）
    void recordMessage(int64_t group_id, const std::string& user_id,
                      const std::string& message, const std::vector<std::string>& recent);

    // 获取群组上下文（群风格 + 推荐行话）
    std::string getGroupContext(int64_t group_id);

    // 反馈（确认/拒绝行话）
    void recordFeedback(int64_t group_id, const std::string& term, const std::string& feedback);

    // LLM 推理行话含义
    void inferJargonMeanings(int64_t group_id, int max_infer = 5);

    // 获取行话详情
    json getJargonData(int64_t group_id) const;

    // 清空群组数据
    void clearGroup(int64_t group_id);

    // 关闭并保存
    void shutdown();

    // 设置推理频率阈值（用于自定义推理触发时机）
    void setInferenceThresholds(const std::vector<int>& thresholds);

    // 应用时间衰减（清理过期词汇）
    void applyTimeDecay(int64_t group_id, int days_threshold = 30);

    // 设置推荐用法返回的最大数量
    void setMaxRecommendations(int max_count);

    // 获取当前推荐用法最大数量
    int getMaxRecommendations() const;

private:
    VocabularyManager() = default;
    bool _initialized = false;
    std::mutex _mutex;
};
