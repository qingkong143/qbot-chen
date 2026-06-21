#include "src/memory/style_learner.h"

StyleLearner& StyleLearner::get() {
    static StyleLearner instance;
    return instance;
}

void StyleLearner::recordMessageStyle(int64_t group_id, const std::string& message, const std::string& username) {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    _groupMessages[group_id].push_back({now, message, username});
    if (_groupMessages[group_id].size() > 500) {
        _groupMessages[group_id].erase(_groupMessages[group_id].begin());
    }
}

void StyleLearner::recordFeedback(int64_t group_id, const std::string& message, bool positive) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::istringstream iss(message);
    std::string word;
    int delta = positive ? 1 : -1;
    while (iss >> word) {
        if (word.size() > 1) _expressionWeights[group_id][word] += delta;
    }
}

std::vector<std::string> StyleLearner::extractHighFreqWords(const std::vector<MessageRecord>& msgs, int topN) {
    std::map<std::string, int> freq;
    for (const auto& msg : msgs) {
        std::istringstream iss(msg.text);
        std::string word;
        while (iss >> word) {
            if (word.size() > 1) freq[word]++;
        }
    }
    std::vector<std::pair<std::string, int>> vec(freq.begin(), freq.end());
    std::sort(vec.begin(), vec.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> result;
    for (int i = 0; i < topN && i < (int)vec.size(); i++) {
        result.push_back(vec[i].first);
    }
    return result;
}

std::string StyleLearner::getRecentTrendInsights(int64_t group_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _groupMessages.find(group_id);
    if (it == _groupMessages.end() || it->second.empty()) return "";

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t sevenDaysAgo = now - 7 * 86400;

    std::vector<MessageRecord> recent;
    for (const auto& msg : it->second) {
        if (msg.ts > sevenDaysAgo) recent.push_back(msg);
    }

    if (recent.empty()) return "";

    auto topWords = extractHighFreqWords(recent, 5);
    std::ostringstream oss;
    oss << "[群风格趋势]\n本周高频词: ";
    for (const auto& word : topWords) {
        oss << word << " ";
    }
    oss << "\n";
    return oss.str();
}
