#include "src/memory/jargon_miner.h"
#include "src/core/base.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

JargonMiner& JargonMiner::get() {
    static JargonMiner instance;
    return instance;
}

void JargonMiner::open(const std::string& path) {
    _filepath = path;
    std::ifstream file(path);
    if (file.good()) {
        try {
            file >> _data;
        } catch (...) {
            _data = json::object();
        }
    } else {
        _data = json::object();
    }
    std::cout << "[行话挖掘] 已打开 " << path << std::endl;
}

void JargonMiner::close() {
    save();
}

void JargonMiner::save() {
    if (_filepath.empty()) return;
    try {
        std::ofstream file(_filepath);
        file << _data.dump(2);
        file.close();
    } catch (...) {
        std::cerr << "[行话挖掘] 保存失败" << std::endl;
    }
}

bool JargonMiner::isNonStandardTerm(const std::string& term) const {
    // 过滤纯标点、纯数字、太长或太短
    if (term.length() < MIN_TERM_LENGTH || term.length() > MAX_TERM_LENGTH) {
        return false;
    }

    // 过滤纯数字或纯英文的混合物（如 CQ 码残留）
    bool hasChineseChar = false;
    bool hasOnlyDigitsAndEnglish = true;

    for (char c : term) {
        // 检测中文字符（UTF-8 编码）
        if ((unsigned char)c > 0x7F) {
            hasChineseChar = true;
            hasOnlyDigitsAndEnglish = false;
            break;
        }
        // 非数字、非字母、非下划线
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '_')) {
            hasOnlyDigitsAndEnglish = false;
        }
    }

    // 纯英文+数字的混合（如 CQatqq3186708635）不算非标准词
    if (hasOnlyDigitsAndEnglish) {
        return false;
    }

    // 必须包含中文才算非标准词（网络词、黑话等）
    if (!hasChineseChar) {
        return false;
    }

    // 已知的常见网络词库
    static const std::set<std::string> commonNetworkTerms = {
        "卧槽", "绝了", "草", "艹", "破", "离谱", "拉", "绷",
        "馋", "馋哥", "哥们", "老哥", "老哥稳", "gkd", "kkp",
        "nm", "yqk", "nsb", "tnnd", "nnd", "sb"
    };

    if (commonNetworkTerms.count(term) > 0) {
        return true;
    }

    // 对包含中文的词进行启发式检测
    // 统计中文字符占比（中文是多字节 UTF-8）
    int chineseCount = 0;
    for (size_t i = 0; i < term.length(); ++i) {
        if ((unsigned char)term[i] > 0x7F) {
            // UTF-8 中文字符通常 3 字节
            chineseCount++;
            i += 2;  // 跳过后续字节
        }
    }

    // 中文占比 >= 60% 才认为是非标准词
    double chineseRatio = (double)chineseCount / term.length();
    return chineseRatio >= 0.6;
}

bool JargonMiner::shouldAnalyzeTerm(const std::string& term, int64_t group_id) const {
    if (!isNonStandardTerm(term)) {
        return false;
    }

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey)) {
        return true;  // 新群，分析所有非标准词
    }

    auto& groupJargons = _data[groupKey];
    if (!groupJargons.is_array()) {
        return true;
    }

    // 已分析过的词，跳过
    for (const auto& entry : groupJargons) {
        if (entry.contains("term") && entry["term"].get<std::string>() == term) {
            return false;
        }
    }
    return true;
}

void JargonMiner::scanMessage(int64_t group_id, const std::string& user_id,
                              const std::string& message, const std::vector<std::string>& recentMessages) {
    // 简单分词（按空格和标点）
    std::vector<std::string> tokens;
    std::istringstream iss(message);
    std::string token;
    while (iss >> token) {
        // 移除标点
        token.erase(std::remove_if(token.begin(), token.end(),
                                   [](char c) { return !isalnum(c) && c != '_' && c != '-'; }),
                    token.end());
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey)) {
        _data[groupKey] = json::array();
    }

    auto& groupJargons = _data[groupKey];
    if (!groupJargons.is_array()) {
        groupJargons = json::array();
    }

    auto now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;

    // 检测新行话
    for (const auto& term : tokens) {
        if (shouldAnalyzeTerm(term, group_id)) {
            // 构造上下文（最近3条消息）
            std::string context;
            int cnt = 0;
            for (auto it = recentMessages.rbegin(); it != recentMessages.rend() && cnt < 3; ++it, ++cnt) {
                if (it->length() > 100) {
                    context += it->substr(0, 100) + "...\n";
                } else {
                    context += *it + "\n";
                }
            }
            context += "[新词]: " + message;

            json entry;
            entry["term"] = term;
            entry["context"] = context;
            entry["user_id"] = user_id;
            entry["timestamp"] = now;
            entry["frequency"] = 1;
            entry["inferred_meaning"] = "";
            entry["inferred_emotion"] = "neutral";
            entry["inferred_usage"] = "";
            entry["confidence"] = 0.0;  // 待 LLM 分析

            groupJargons.push_back(entry);
            std::cout << "[行话挖掘] 群 " << group_id << " 发现新词: " << term << std::endl;
        } else {
            // 更新频率
            for (auto& entry : groupJargons) {
                if (entry.contains("term") && entry["term"].get<std::string>() == term) {
                    if (entry.contains("frequency")) {
                        entry["frequency"] = entry["frequency"].get<int>() + 1;
                    }
                    break;
                }
            }
        }
    }

    // 保持数组大小（最多记录50条）
    if (groupJargons.size() > 50) {
        groupJargons.erase(groupJargons.begin());
    }

    save();
}

json JargonMiner::getGroupJargons(int64_t group_id) const {
    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return json::array();
    }
    return _data[groupKey];
}

std::string JargonMiner::getRecommendedUsage(int64_t group_id) const {
    auto jargons = getGroupJargons(group_id);
    if (jargons.empty()) {
        return "";
    }

    // 收集已分析且置信度高的行话
    std::vector<std::string> recommendations;
    for (const auto& entry : jargons) {
        if (entry.contains("inferred_meaning") && !entry["inferred_meaning"].get<std::string>().empty()) {
            double conf = entry.contains("confidence") ? entry["confidence"].get<double>() : 0.0;
            if (conf > 0.6) {
                std::string term = entry["term"].get<std::string>();
                std::string meaning = entry["inferred_meaning"].get<std::string>();
                recommendations.push_back(term + "(" + meaning + ")");
            }
        }
    }

    if (recommendations.empty()) {
        return "";
    }

    // 返回最近使用频率最高的3个
    std::string result = "[群内行话参考]\n";
    int cnt = 0;
    for (const auto& r : recommendations) {
        if (cnt >= 3) break;
        result += "- " + r + "\n";
        cnt++;
    }
    return result;
}

void JargonMiner::recordFeedback(int64_t group_id, const std::string& term,
                                 const std::string& feedback) {
    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return;
    }

    auto& groupJargons = _data[groupKey];
    for (auto& entry : groupJargons) {
        if (entry.contains("term") && entry["term"].get<std::string>() == term) {
            if (feedback == "positive") {
                double conf = entry.contains("confidence") ? entry["confidence"].get<double>() : 0.0;
                entry["confidence"] = std::min(1.0, conf + 0.1);
                entry["inferred_emotion"] = "positive";
            } else if (feedback == "negative") {
                double conf = entry.contains("confidence") ? entry["confidence"].get<double>() : 0.0;
                entry["confidence"] = std::max(0.0, conf - 0.15);
                entry["inferred_emotion"] = "negative";
            }
            break;
        }
    }

    save();
}
