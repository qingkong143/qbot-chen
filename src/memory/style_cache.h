#pragma once
#include "src/core/base.h"
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>

using json = nlohmann::json;

// 群风格缓存管理器（单例）
// 管理每个群的风格分析结果，从 JSON 文件持久化
class StyleCache {
public:
	static StyleCache& get();

	// 从文件加载缓存
	bool load(const std::string& path = "group_style_cache.json");

	// 保存当前缓存到文件
	bool save();

	// 获取群的群风格字符串（JSON key-value 格式）
	// 返回空字符串表示无缓存或需要更新
	std::string get(int64_t group_id) const;

	// 设置群风格数据
	void set(int64_t group_id, json style_data);

	// 使缓存失效（下次 get() 时触发重新分析）
	void invalidate(int64_t group_id);

	// 清空所有缓存
	void clear();

	// 检查缓存是否需要更新（基于时间）
	bool isExpired(int64_t group_id, int expireSeconds = 604800) const;  // 默认 7 天

private:
	StyleCache() = default;
	~StyleCache() = default;

	struct CacheEntry {
		json data;
		std::chrono::system_clock::time_point timestamp;
	};

	std::unordered_map<int64_t, CacheEntry> _cache;
	std::string _filePath;
	mutable std::mutex _mutex;
};
