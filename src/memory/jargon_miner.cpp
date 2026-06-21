#include "src/memory/jargon_miner.h"
#include "src/memory/jargon_inference.h"
#include "src/infra/logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <regex>
#include <set>

JargonMiner& JargonMiner::get() {
    static JargonMiner instance;
    return instance;
}

void JargonMiner::initialize(const std::string& path) {
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
    Logger::get().info("[行话挖掘]", "已初始化，数据文件: " + path);
}

void JargonMiner::save() {
    if (_filepath.empty()) return;
    try {
        std::ofstream file(_filepath);
        if (!file.is_open()) {
            Logger::get().error("[行话挖掘]", "无法打开文件: " + _filepath);
            return;
        }
        file << _data.dump(2);
        file.close();
        Logger::get().debug("[行话挖掘]", "已保存数据");
    } catch (const std::exception& e) {
        Logger::get().error("[行话挖掘]", "保存失败: " + std::string(e.what()));
    }
}

std::vector<std::string> JargonMiner::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    std::istringstream iss(text);
    std::string token;

    while (iss >> token) {
        size_t start = 0, end = token.length();

        // 移除前后标点
        while (start < end && !std::isalnum(token[start])) start++;
        while (end > start && !std::isalnum(token[end-1])) end--;

        if (start < end) {
            token = token.substr(start, end - start);
            if (!token.empty() && !isNoise(token)) {
                tokens.push_back(token);
            }
        }
    }
    return tokens;
}

bool JargonMiner::isNoise(const std::string& term) const {
    if (term.length() < MIN_TERM_LENGTH || term.length() > MAX_TERM_LENGTH) return true;

    // 纯数字
    bool allDigits = std::all_of(term.begin(), term.end(), ::isdigit);
    if (allDigits) return true;

    // 纯英文单字母
    bool allEnglish = std::all_of(term.begin(), term.end(),
        [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); });
    if (allEnglish && term.length() < 3) return true;

    // CQ 码残留
    if (term.find("CQ") == 0 || term.find("cq") == 0) return true;

    return false;
}

void JargonMiner::scanMessage(int64_t group_id, const std::string& user_id,
                             const std::string& message,
                             const std::vector<std::string>& recent_messages) {
    std::lock_guard<std::mutex> lock(_data_mutex);

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey)) {
        _data[groupKey] = json::array();
    }

    auto& groupJargons = _data[groupKey];
    if (!groupJargons.is_array()) {
        groupJargons = json::array();
    }

    // 防止数据膨胀：如果达到限制，清理低频词汇
    if (groupJargons.size() >= MAX_ENTRIES_PER_GROUP) {
        // 按频率排序并删除最低频的 20% 词汇
        std::vector<std::pair<int, size_t>> freq_index;  // (frequency, index)
        for (size_t i = 0; i < groupJargons.size(); i++) {
            int count = groupJargons[i].value("count", 0);
            freq_index.push_back({count, i});
        }

        // 按频率升序排序
        std::sort(freq_index.begin(), freq_index.end());

        // 删除最低频的 20%
        size_t to_remove = MAX_ENTRIES_PER_GROUP / 5;
        std::vector<size_t> indices_to_remove;
        for (size_t i = 0; i < to_remove && i < freq_index.size(); i++) {
            indices_to_remove.push_back(freq_index[i].second);
        }

        // 反向排序索引以保持有效性
        std::sort(indices_to_remove.rbegin(), indices_to_remove.rend());

        for (size_t idx : indices_to_remove) {
            groupJargons.erase(groupJargons.begin() + idx);
        }

        Logger::get().info("[行话挖掘]", "群 " + std::to_string(group_id) +
                          " 数据膨胀处理: 清理了 " + std::to_string(to_remove) + " 个低频词汇");
    }

    auto tokens = tokenize(message);
    if (tokens.empty()) {
        return;  // 提前返回，避免不必要处理
    }

    // 收集原始上下文
    std::vector<std::string> raw_content_list = recent_messages;
    raw_content_list.push_back("[新消息]: " + message);

    // 构建 token -> entry 映射表，加速查找
    std::map<std::string, json*> token_map;
    for (auto& entry_json : groupJargons) {
        if (entry_json.contains("content")) {
            token_map[entry_json["content"].get<std::string>()] = &entry_json;
        }
    }

    // 对每个词进行处理
    for (const auto& token : tokens) {
        auto it = token_map.find(token);
        if (it != token_map.end()) {
            // 词汇已存在，更新计数和时间戳
            (*it->second)["count"] = (*it->second)["count"].get<int>() + 1;
            (*it->second)["last_seen_timestamp"] = (long)time(nullptr);

            // 合并新上下文（保持最近 MAX_CONTEXT_WINDOW 条消息）
            try {
                auto existing_raw = json::parse((*it->second)["raw_content"].get<std::string>());
                if (existing_raw.is_array()) {
                    // 合并旧上下文和新上下文，去重后限制窗口大小
                    std::vector<std::string> merged;
                    for (const auto& msg : existing_raw) {
                        merged.push_back(msg.get<std::string>());
                    }
                    for (const auto& msg : raw_content_list) {
                        // 简单去重：不添加完全相同的消息
                        if (std::find(merged.begin(), merged.end(), msg) == merged.end()) {
                            merged.push_back(msg);
                        }
                    }
                    // 只保留最近 MAX_CONTEXT_WINDOW 条
                    if (merged.size() > MAX_CONTEXT_WINDOW) {
                        merged.erase(merged.begin(), merged.end() - MAX_CONTEXT_WINDOW);
                    }
                    (*it->second)["raw_content"] = json(merged).dump();
                }
            } catch (...) {
                // 如果合并失败，保持原来的上下文
            }

            // 检查是否需要触发推理
            JargonData jargon = JargonData::from_json(*it->second);
            if (shouldInfer(jargon)) {
                triggerInference(group_id, token);
            }
        } else {
            // 新词汇，添加到列表
            JargonData new_jargon;
            new_jargon.content = token;
            new_jargon.count = 1;
            new_jargon.created_by = JargonCreatedBy::AI;
            new_jargon.raw_content = json(raw_content_list).dump();

            groupJargons.push_back(new_jargon.to_json());
            Logger::get().debug("[行话挖掘]", "群 " + std::to_string(group_id) + " 收集新词: " + token);
        }
    }
    // 注意：save() 已被移除，改为批量保存。调用方需在适当时机调用 flushToFile()}
}

std::string JargonMiner::getRecommendedUsage(int64_t group_id) {
    std::lock_guard<std::mutex> lock(_data_mutex);

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return "";
    }

    auto& groupJargons = _data[groupKey];
    std::vector<std::string> recommendations;

    for (const auto& entry : groupJargons) {
        if (entry.contains("is_jargon") && entry["is_jargon"].is_boolean() && entry["is_jargon"].get<bool>()) {
            if (entry.contains("meaning") && !entry["meaning"].get<std::string>().empty()) {
                std::string term = entry["content"].get<std::string>();
                std::string meaning = entry["meaning"].get<std::string>();
                recommendations.push_back(term + "(" + meaning + ")");
            }
        }
    }

    if (recommendations.empty()) return "";

    std::string result = "[群内行话参考]\n";
    int cnt = 0;
    for (const auto& r : recommendations) {
        if (cnt >= _max_recommendations) break;  // 使用可配置的限制
        result += "- " + r + "\n";
        cnt++;
    }
    return result;
}

std::vector<JargonData> JargonMiner::getGroupJargons(int64_t group_id) {
    std::lock_guard<std::mutex> lock(_data_mutex);

    std::string groupKey = std::to_string(group_id);
    std::vector<JargonData> result;

    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return result;
    }

    for (const auto& entry : _data[groupKey]) {
        result.push_back(JargonData::from_json(entry));
    }
    return result;
}

json JargonMiner::getAllJargons() const {
    return _data;
}

void JargonMiner::recordFeedback(int64_t group_id, const std::string& term,
                                const std::string& feedback) {
    std::lock_guard<std::mutex> lock(_data_mutex);

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return;
    }

    for (auto& entry : _data[groupKey]) {
        if (entry.contains("content") && entry["content"].get<std::string>() == term) {
            if (feedback == "confirmed") {
                entry["is_jargon"] = true;
            } else if (feedback == "rejected") {
                entry["is_jargon"] = false;
            }
            break;
        }
    }
    // 注意：save() 已被移除，改为批量保存。调用方需在适当时机调用 flushToFile()
}

bool JargonMiner::shouldInfer(const JargonData& jargon) const {
    // 使用可配置的阈值（默认是 {4, 8, 25, 100}）
    std::set<int> thresholds(_inference_thresholds.begin(), _inference_thresholds.end());
    return thresholds.count(jargon.count) > 0 && jargon.last_inference_count < jargon.count;
}

void JargonMiner::triggerInference(int64_t group_id, const std::string& term) {
    std::lock_guard<std::mutex> lock(_data_mutex);

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return;
    }

    auto& groupJargons = _data[groupKey];

    // 线性查找词汇（数据量不大时可接受）
    for (auto& entry : groupJargons) {
        if (entry.contains("content") && entry["content"].get<std::string>() == term) {
            JargonData jargon = JargonData::from_json(entry);

            // 获取群组最近消息作为上下文
            std::vector<std::string> recent_msgs;
            try {
                auto raw = json::parse(jargon.raw_content);
                if (raw.is_array()) {
                    for (const auto& msg : raw) {
                        recent_msgs.push_back(msg.get<std::string>());
                    }
                }
            } catch (...) {}

            // 执行 3 层推理
            auto inference_result = JargonInference::get().infer(jargon, recent_msgs, _deepseek, _curl);

            // 更新结果
            entry["is_jargon"] = inference_result.is_jargon;
            entry["meaning"] = inference_result.meaning;
            entry["last_inference_count"] = jargon.count;

            // 达到 100 后标记为完成
            if (jargon.count >= 100) {
                entry["is_complete"] = true;
            }

            Logger::get().info("[行话挖掘]", "词汇 '" + term + "' 推理完成, is_jargon=" +
                              (inference_result.is_jargon ? "true" : "false"));
            break;
        }
    }

    save();
}

void JargonMiner::applyTimeDecay(int64_t group_id, int days_threshold) {
    std::lock_guard<std::mutex> lock(_data_mutex);

    std::string groupKey = std::to_string(group_id);
    if (!_data.contains(groupKey) || !_data[groupKey].is_array()) {
        return;
    }

    auto& groupJargons = _data[groupKey];
    time_t now = time(nullptr);
    time_t threshold_seconds = days_threshold * 86400;  // 转换天数为秒
    int decayed_count = 0;

    // 遍历词汇，应用时间衰减
    auto it = groupJargons.begin();
    while (it != groupJargons.end()) {
        if (it->contains("last_seen_timestamp")) {
            time_t last_seen = it->value("last_seen_timestamp", now);
            time_t age = now - last_seen;

            if (age > threshold_seconds) {
                // 超过阈值的词汇删除
                it = groupJargons.erase(it);
                decayed_count++;
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }

    if (decayed_count > 0) {
        Logger::get().info("[行话挖掘]", "群 " + std::to_string(group_id) +
                          " 时间衰减: 清理了 " + std::to_string(decayed_count) + " 个过期词汇");
        // 注意：save() 已被移除，改为批量保存。调用方需在适当时机调用 flushToFile()
    }
}
