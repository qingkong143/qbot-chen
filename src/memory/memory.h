#pragma once
#include "src/core/base.h"

// ── SQLite 持久化记忆（单例） ──────────────────
class Memory {
public:
	static Memory& get();

	bool open(const std::string& path = "messages.db");
	void close();

	// 存储一条消息
	void store(int64_t chat_id, const std::string& chat_type,
		int64_t sender_id, const std::string& sender_name,
		const std::string& content, bool is_bot);

	// 获取指定会话最近 N 条消息
	std::string recent(int64_t chat_id, const std::string& chat_type, int limit = 30);

	// 获取指定会话最近 N 条适合注入 prompt 的文本上下文
	std::string recentTextContext(int64_t chat_id, const std::string& chat_type,
		int limit = 8, int64_t exclude_sender_id = 0,
		const std::string& exclude_content = "");

	// 按条数+绝对时间范围查询（from_ts/to_ts=0不限；keyword非空则全文搜索）
	std::string query(int64_t chat_id, const std::string& chat_type,
		int count, int64_t from_ts = 0, int64_t to_ts = 0,
		const std::string& keyword = "");

	// 全文搜索历史消息（关键词 + 可选范围）
	std::string search(const std::string& keyword, int64_t chat_id = 0,
		const std::string& chat_type = "", int limit = 20);

	// 按天数范围取最近消息（如最近7天），返回分析友好格式
	std::string recentByDays(int64_t chat_id, const std::string& chat_type,
		int days, int max_messages = 200);

	// 取群消息用于群风格分析：返回 [昵称(QQ)]: 内容\n 格式
	std::string getGroupStyleQuery(int64_t group_id, int days = 7);

private:
	Memory() = default;
	~Memory() { close(); }
	void* _db = nullptr;
	int _storeCnt = 0;
};
