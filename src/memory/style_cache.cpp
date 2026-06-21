#include "src/memory/style_cache.h"
#include <fstream>
#include <iostream>

StyleCache& StyleCache::get() {
	static StyleCache instance;
	return instance;
}

bool StyleCache::load(const std::string& path) {
	_filePath = path;
	std::lock_guard<std::mutex> lock(_mutex);

	std::ifstream ifs(path);
	if (!ifs.is_open()) {
		std::cout << "[风格缓存] 无缓存文件，将创建: " << path << std::endl;
		return true;  // 无文件不算失败
	}

	try {
		json j;
		ifs >> j;
		if (j.is_object()) {
			auto now = std::chrono::system_clock::now();
			for (auto& [key, val] : j.items()) {
				if (val.is_object()) {
					int64_t gid;
					try { gid = std::stoll(key); }
					catch (...) { continue; }
					CacheEntry entry;
					entry.data = val;
					entry.timestamp = now;  // 加载时设置为当前时间
					_cache[gid] = entry;
				}
			}
		}
		std::cout << "[风格缓存] 已加载 " << _cache.size() << " 个群的风格" << std::endl;
		return true;
	} catch (const std::exception& e) {
		std::cerr << "[风格缓存] 加载失败: " << e.what() << std::endl;
		return false;
	}
}

bool StyleCache::save() {
	if (_filePath.empty()) return false;

	std::lock_guard<std::mutex> lock(_mutex);
	json out = json::object();
	for (auto& [gid, entry] : _cache) {
		out[std::to_string(gid)] = entry.data;
	}

	std::ofstream ofs(_filePath);
	if (!ofs.is_open()) {
		std::cerr << "[风格缓存] 保存失败: " << _filePath << std::endl;
		return false;
	}
	ofs << out.dump(2);
	ofs.close();
	std::cout << "[风格缓存] 已保存 " << _cache.size() << " 个群的风格" << std::endl;
	return true;
}

std::string StyleCache::get(int64_t group_id) const {
	std::lock_guard<std::mutex> lock(_mutex);
	auto it = _cache.find(group_id);
	if (it == _cache.end()) return "";

	const auto& data = it->second.data;
	auto getStringField = [](const json& obj, const std::string& key, const std::string& fallback) {
		if (obj.contains(key) && obj[key].is_string() && !obj[key].get<std::string>().empty()) {
			return obj[key].get<std::string>();
		}
		return fallback;
	};

	std::string result = "常用语气词：" + getStringField(data, "keywords", "无") + "\n";
	result += "群风格：" + getStringField(data, "tone", "normal") + "\n";

	auto appendOptional = [&](const std::string& key, const std::string& label) {
		if (data.contains(key) && data[key].is_string() && !data[key].get<std::string>().empty()) {
			result += label;
			result += data[key].get<std::string>();
			result += "\n";
		}
	};

	appendOptional("catchphrases", "口头禅：");
	appendOptional("emoji_pref", "表情包偏好：");
	appendOptional("taboo", "禁忌话题：");
	appendOptional("greetings", "打招呼方式：");
	appendOptional("slang", "群内词语理解：");
	appendOptional("response_patterns", "场景回应方式：");
	appendOptional("positive_feedback", "正反馈信号：");
	appendOptional("negative_feedback", "负反馈信号：");
	appendOptional("boundaries", "学习边界：");

	return result;
}

void StyleCache::set(int64_t group_id, json style_data) {
	if (!style_data.is_object()) return;
	if (!style_data.contains("keywords") || !style_data["keywords"].is_string() || style_data["keywords"].get<std::string>().empty()) {
		style_data["keywords"] = "无";
	}
	if (!style_data.contains("tone") || !style_data["tone"].is_string() || style_data["tone"].get<std::string>().empty()) {
		style_data["tone"] = "normal";
	}
	style_data["version"] = style_data.value("version", 0) + 1;
	style_data["updated"] = std::to_string(
		std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count());

	std::lock_guard<std::mutex> lock(_mutex);
	CacheEntry entry;
	entry.data = style_data;
	entry.timestamp = std::chrono::system_clock::now();
	_cache[group_id] = entry;
}

void StyleCache::invalidate(int64_t group_id) {
	std::lock_guard<std::mutex> lock(_mutex);
	_cache.erase(group_id);
}

void StyleCache::clear() {
	std::lock_guard<std::mutex> lock(_mutex);
	_cache.clear();
}

bool StyleCache::isExpired(int64_t group_id, int expireSeconds) const {
	std::lock_guard<std::mutex> lock(_mutex);
	auto it = _cache.find(group_id);
	if (it == _cache.end()) return true;  // 不存在则视为过期

	auto now = std::chrono::system_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();
	return elapsed >= expireSeconds;
}
