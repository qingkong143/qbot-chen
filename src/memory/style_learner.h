#pragma once
#include "src/core/base.h"
#include <mutex>

class StyleLearner {
public:
    static StyleLearner& get();

    void recordMessageStyle(int64_t group_id, const std::string& message, const std::string& username);
    void recordFeedback(int64_t group_id, const std::string& message, bool positive);
    std::string getRecentTrendInsights(int64_t group_id);

private:
    StyleLearner() = default;
    ~StyleLearner() = default;

    struct MessageRecord {
        int64_t ts;
        std::string text;
        std::string user;
    };

    std::map<int64_t, std::vector<MessageRecord>> _groupMessages;
    std::map<int64_t, std::map<std::string, int>> _expressionWeights;
    std::mutex _mutex;

    std::vector<std::string> extractHighFreqWords(const std::vector<MessageRecord>& msgs, int topN = 10);
};
