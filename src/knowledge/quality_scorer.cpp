#include "src/knowledge/quality_scorer.h"
#include "src/infra/logger.h"
#include <cmath>
#include <algorithm>
#include <chrono>

QualityScorer& QualityScorer::get() {
    static QualityScorer instance;
    return instance;
}

void QualityScorer::set_config(const ScoringConfig& config) {
    std::lock_guard<std::mutex> lock(_mutex);
    _config = config;
    Logger::get().info("[质量评分] ", "配置已更新");
}

QualityScorer::ScoringConfig QualityScorer::get_config() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

float QualityScorer::_score_frequency(int frequency) {
    // 对数函数：1-5 次 = 0.5-0.8，5+ 次 = 0.8-1.0
    if (frequency <= 0) return 0.0f;
    return std::min(1.0f, 0.5f + 0.3f * (float)std::log(frequency + 1) / (float)std::log(10));
}

float QualityScorer::_score_recency(int64_t timestamp, int freshness_days) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
    int64_t age_seconds = now - timestamp;
    int age_days = age_seconds / 86400;

    if (age_days <= 0) return 1.0f;
    if (age_days > freshness_days) return 0.5f;

    // 线性衰减：新鲜 = 1.0，超过 freshness_days = 0.5
    return 1.0f - (age_days / (float)freshness_days) * 0.5f;
}

float QualityScorer::_normalize_similarity(float similarity) {
    // 将相似度从 [-1, 1] 或 [0, 1] 规范化到 [0, 1]
    return std::max(0.0f, std::min(1.0f, (similarity + 1.0f) / 2.0f));
}

float QualityScorer::score_knowledge(const std::string& content, int frequency, int64_t timestamp) {
    auto config = get_config();

    // 计算各维度得分
    float freq_score = _score_frequency(frequency) * config.frequency_weight;
    float recency_score = _score_recency(timestamp, config.freshness_days) * config.recency_weight;

    // 基础得分 + 频率 + 新鲜度
    float base = config.base_score * (1.0f - config.frequency_weight - config.recency_weight);
    float score = base + freq_score + recency_score;

    // 检查是否有用户反馈
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _feedback_history.find(content);
        if (it != _feedback_history.end()) {
            const auto& stats = it->second;
            float feedback_score = stats.score * config.feedback_weight;
            score = score * (1.0f - config.feedback_weight) + feedback_score;
        }
    }

    return std::min(1.0f, std::max(0.0f, score));
}

void QualityScorer::record_feedback(const std::string& knowledge_hash, const std::string& feedback) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto& stats = _feedback_history[knowledge_hash];

    if (feedback == "helpful") {
        stats.helpful_count++;
    } else if (feedback == "unhelpful") {
        stats.unhelpful_count++;
    }

    // 计算综合反馈分数：helpful/(helpful+unhelpful)
    int total = stats.helpful_count + stats.unhelpful_count;
    if (total > 0) {
        stats.score = stats.helpful_count / (float)total;
    } else {
        stats.score = 0.5f;  // 无反馈时回到中性
    }

    Logger::get().debug("[质量评分] ", "反馈记录: " + knowledge_hash + " -> " +
                       (feedback == "helpful" ? "有用" : "无用") +
                       " (有用:" + std::to_string(stats.helpful_count) +
                       ", 无用:" + std::to_string(stats.unhelpful_count) + ")");
}

float QualityScorer::get_score(const std::string& knowledge_hash) const {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _feedback_history.find(knowledge_hash);
    if (it != _feedback_history.end()) {
        return it->second.score;
    }

    return get_config().base_score;
}

QualityScorer::FeedbackStats QualityScorer::get_feedback_stats(const std::string& knowledge_hash) const {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _feedback_history.find(knowledge_hash);
    if (it != _feedback_history.end()) {
        return it->second;
    }

    return FeedbackStats();
}

std::vector<std::pair<std::string, float>> QualityScorer::rank_by_quality(
    const std::vector<std::pair<std::string, float>>& search_results,
    bool use_feedback) {

    std::vector<std::pair<std::string, float>> ranked = search_results;

    if (use_feedback) {
        // 按用户反馈分数排序
        std::sort(ranked.begin(), ranked.end(),
            [this](const auto& a, const auto& b) {
                float score_a = this->get_score(a.first);
                float score_b = this->get_score(b.first);
                return score_a > score_b;
            }
        );
    }

    Logger::get().debug("[质量评分] ", "排序 " + std::to_string(ranked.size()) +
                       " 条搜索结果 (use_feedback=" + (use_feedback ? "true" : "false") + ")");

    return ranked;
}
