#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>
#include "src/memory/jargon_data_model.h"
#include "src/bot/deepseek.h"
#include <curl/curl.h>

using json = nlohmann::json;

class VocabularyManager;

// 行话挖掘系统（扫描消息 + 频率阈值触发推理）
class JargonMiner {
    friend class VocabularyManager;
public:
    static JargonMiner& get();

    // 初始化（加载或创建行话数据文件）
    void initialize(const std::string& path = "jargons.json");

    // 扫描消息并收集词汇
    void scanMessage(int64_t group_id, const std::string& user_id,
                    const std::string& message, const std::vector<std::string>& recent_messages);

    // 获取推荐用法（用于回复注入）
    std::string getRecommendedUsage(int64_t group_id);

    // 获取群组所有行话
    std::vector<JargonData> getGroupJargons(int64_t group_id);

    // 获取所有行话（用于清理等）
    json getAllJargons() const;

    // 人工反馈
    void recordFeedback(int64_t group_id, const std::string& term, const std::string& feedback);

    // 手动触发行话推理（用于 VocabularyManager）
    void triggerInference(int64_t group_id, const std::string& term);

    // 设置 Deepseek 和 CURL 实例（用于 LLM 推理）
    void setLLMDependencies(Deepseek* deepseek, CURL* curl) {
        _deepseek = deepseek;
        _curl = curl;
    }

    // 设置推理频率阈值（用于自定义推理时机）
    void setInferenceThresholds(const std::vector<int>& thresholds) {
        _inference_thresholds = thresholds;
    }

    // 获取当前推理频率阈值
    const std::vector<int>& getInferenceThresholds() const {
        return _inference_thresholds;
    }

    // 手动触发数据保存（用于批量操作后）
    void flushToFile() {
        std::lock_guard<std::mutex> lock(_data_mutex);
        save();
    }

    // 设置推荐用法返回的最大数量
    void setMaxRecommendations(int max_count) {
        _max_recommendations = max_count;
    }

    // 获取当前推荐用法最大数量
    int getMaxRecommendations() const {
        return _max_recommendations;
    }

private:
    JargonMiner() = default;
    std::string _filepath;
    json _data;  // { group_id: [jargon1, jargon2, ...], ... }
    Deepseek* _deepseek = nullptr;
    CURL* _curl = nullptr;
    std::vector<int> _inference_thresholds = {4, 8, 25, 100};  // 可配置的推理阈值
    int _max_recommendations = 5;  // 推荐用法返回的最大数量
    mutable std::mutex _data_mutex;  // 保护 _data 访问

    static constexpr int MIN_TERM_LENGTH = 2;
    static constexpr int MAX_TERM_LENGTH = 20;
    static constexpr int MAX_ENTRIES_PER_GROUP = 500;
    static constexpr int MAX_CONTEXT_WINDOW = 10;  // 保存最近 10 条消息作为上下文

    // 频率阈值（达到这些阈值时触发 LLM 推理）
    static constexpr int INFERENCE_THRESHOLDS[] = {4, 8, 25, 100};

    void save();
    std::vector<std::string> tokenize(const std::string& text) const;
    bool isNoise(const std::string& term) const;

    // 检查是否需要触发推理（基于频率阈值）
    bool shouldInfer(const JargonData& jargon) const;

    // 应用时间衰减到群组词汇（清理过期词汇）
    // days_threshold: 多少天未见过的词汇会被衰减或删除
    void applyTimeDecay(int64_t group_id, int days_threshold = 30);
};
