#include "src/infra/message_deduplicator.h"
#include <algorithm>
#include <chrono>

MessageDeduplicator::MessageDeduplicator()
    : _config() {}

MessageDeduplicator::MessageDeduplicator(const DeduplicatorConfig& config)
    : _config(config) {}

int64_t MessageDeduplicator::getCurrentTimestamp() {
    return std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
}

std::string MessageDeduplicator::stripAtPrefix(const std::string& message) {
    static const std::regex at_prefix(R"(^\s*(?:\[CQ:at,qq=\d+\]\s*)+)");
    return std::regex_replace(message, at_prefix, "");
}

std::string MessageDeduplicator::normalizeMessage(const std::string& message) {
    size_t start = message.find_first_not_of(" \t\n\r");
    size_t end = message.find_last_not_of(" \t\n\r");

    if (start == std::string::npos) {
        return "";
    }

    return message.substr(start, end - start + 1);
}

bool MessageDeduplicator::isAtOnlyMessage(const std::string& message) const {
    std::string normalized = normalizeMessage(message);
    if (normalized.empty()) {
        return true;
    }

    std::string without_at = stripAtPrefix(normalized);
    size_t start = without_at.find_first_not_of(" \t\n\r，,。！？!?~～·、:：;；");
    return start == std::string::npos;
}


void MessageDeduplicator::cleanupExpiredHistory() {
    int64_t now = getCurrentTimestamp();
    int64_t cutoff = now - _config.time_window_seconds;

    // 移除过期的消息
    while (!_history.empty() && _history.front().timestamp < cutoff) {
        _history.pop_front();
    }
}

bool MessageDeduplicator::shouldProcess(const std::string& message, const std::string& user_id) {
    std::string normalized = normalizeMessage(message);

    if (normalized.length() < _config.min_content_length) {
        return false;
    }

    if (_config.filter_at_only && isAtOnlyMessage(normalized)) {
        return false;
    }

    std::string compare_message = stripAtPrefix(normalized);
    if (compare_message.empty()) {
        compare_message = normalized;
    }

    cleanupExpiredHistory();

    for (const auto& entry : _history) {
        if (entry.user_id == user_id && stripAtPrefix(entry.message) == compare_message) {
            return false;
        }
    }

    return true;
}

void MessageDeduplicator::addMessage(const std::string& message, const std::string& user_id) {
    std::string normalized = normalizeMessage(message);

    if (normalized.length() < _config.min_content_length) {
        return;
    }

    HistoryEntry entry;
    entry.message = normalized;
    entry.user_id = user_id;
    entry.timestamp = getCurrentTimestamp();

    _history.push_back(entry);

    // 限制历史大小
    while (_history.size() > _config.max_history) {
        _history.pop_front();
    }

    // 定期清理过期记录
    if (_history.size() % 10 == 0) {
        cleanupExpiredHistory();
    }
}

void MessageDeduplicator::clear() {
    _history.clear();
}

void MessageDeduplicator::setConfig(const DeduplicatorConfig& config) {
    _config = config;
}
