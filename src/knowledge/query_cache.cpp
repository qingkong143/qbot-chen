#include "src/knowledge/query_cache.h"

QueryCache& QueryCache::get() {
    static QueryCache instance;
    return instance;
}

std::string QueryCache::makeKey(int64_t user_id, int64_t chat_id) {
    return std::to_string(user_id) + "_" + std::to_string(chat_id);
}

bool QueryCache::isExpired(const CacheEntry& entry) {
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.timestamp).count();
    return elapsed > _ttl_seconds;
}

std::string QueryCache::queryProfile(int64_t user_id, int64_t chat_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string key = makeKey(user_id, chat_id);
    auto it = _cache.find(key);
    if (it != _cache.end() && !isExpired(it->second)) {
        return it->second.value;
    }
    return "";
}

void QueryCache::cacheProfile(int64_t user_id, int64_t chat_id, const std::string& profile) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::string key = makeKey(user_id, chat_id);
    _cache[key] = {profile, std::chrono::system_clock::now()};
}

void QueryCache::clearCache() {
    std::lock_guard<std::mutex> lock(_mutex);
    _cache.clear();
}

void QueryCache::setTTL(int seconds) {
    std::lock_guard<std::mutex> lock(_mutex);
    _ttl_seconds = seconds;
}
