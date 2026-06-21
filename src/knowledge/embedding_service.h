#pragma once
#include "src/core/base.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 嵌入结果结构
struct EmbeddingResult {
    std::vector<float> embedding;
    std::string model_name;
    bool success = false;
    std::string error_msg;
};

// ========== 新增：缓存配置 ==========
struct CacheConfig {
    static constexpr size_t DEFAULT_MAX_ENTRIES = 10000;      // 最多10000条
    static constexpr size_t DEFAULT_MAX_SIZE_MB = 100;        // 最多100MB
    static constexpr int64_t DEFAULT_TTL_SECONDS = 86400;     // 24小时TTL
};

// ========== 新增：缓存条目 ==========
struct CacheEntry {
    EmbeddingResult result;
    int64_t access_time;      // 最后访问时间（秒）
    int64_t create_time;      // 创建时间（秒）
    int access_count;         // 访问计数（用于LRU）
};

// 嵌入服务客户端（门面模式）
class EmbeddingServiceClient {
public:
    static EmbeddingServiceClient& get();

    // 配置初始化
    void initialize(const std::string& api_url, const std::string& api_key,
                   const std::string& model = "text-embedding-3-small");

    // 同步接口（单条文本）
    EmbeddingResult embed_text_sync(const std::string& text);

    // 同步接口（批量文本）
    std::vector<EmbeddingResult> embed_texts_sync(const std::vector<std::string>& texts, int max_concurrent = 5);

    // ========== 新增：缓存管理接口 ==========
    /**
     * 清空所有缓存
     */
    void clear_cache();

    /**
     * 设置缓存最大条目数
     */
    void set_cache_max_entries(size_t max_entries);

    /**
     * 设置缓存最大大小（MB）
     */
    void set_cache_max_size_mb(size_t max_size_mb);

    /**
     * 获取当前缓存条目数
     */
    size_t get_cache_entries_count() const;

    /**
     * 获取当前缓存大小（字节）
     */
    size_t get_cache_size() const;

    /**
     * 打印缓存统计信息
     */
    void print_cache_stats();

private:
    EmbeddingServiceClient()
        : _cache_max_entries(CacheConfig::DEFAULT_MAX_ENTRIES),
          _cache_max_size_bytes(CacheConfig::DEFAULT_MAX_SIZE_MB * 1024 * 1024),
          _cache_ttl_seconds(CacheConfig::DEFAULT_TTL_SECONDS) {}

    std::string _api_url;
    std::string _api_key;
    std::string _model;

    // ========== 新增：改进的缓存管理 ==========
    std::unordered_map<std::string, CacheEntry> _cache;
    mutable std::mutex _cache_mutex;
    size_t _cache_max_entries;
    size_t _cache_max_size_bytes;
    int64_t _cache_ttl_seconds;

    // 缓存统计
    size_t _total_cache_hits = 0;
    size_t _total_cache_misses = 0;

    EmbeddingResult _call_api(const std::string& text);
    std::string _hash_text(const std::string& text) const;

    // ========== 新增：缓存管理方法 ==========
    /**
     * 清除过期的缓存条目
     */
    void _evict_expired_entries();

    /**
     * 清除LRU条目以腾出空间
     */
    void _evict_lru_entries();

    /**
     * 估算单个缓存条目的大小
     */
    static size_t _estimate_entry_size(const EmbeddingResult& result);

    /**
     * 估算缓存总大小
     */
    size_t _estimate_cache_size() const;

    /**
     * 检查缓存是否需要清理
     */
    bool _should_evict() const;

    /**
     * 获取当前时间戳（秒）
     */
    static int64_t _get_current_timestamp();
};

