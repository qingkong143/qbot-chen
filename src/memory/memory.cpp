#include "src/memory/memory.h"
#include <sqlite3.h>

namespace {
	bool isAsciiSpace(char c) {
		return c == ' ' || c == '\t' || c == '\r' || c == '\n';
	}

	bool isUtf8ContinuationByte(unsigned char c) {
		return (c & 0xC0) == 0x80;
	}

	std::string sanitizeUtf8Text(const std::string& input) {
		std::string out;
		out.reserve(input.size());
		for (size_t i = 0; i < input.size();) {
			unsigned char c = static_cast<unsigned char>(input[i]);
			size_t extra = 0;
			bool valid = false;

			if ((c & 0x80) == 0x00) {
				extra = 0;
				valid = true;
			} else if ((c & 0xE0) == 0xC0) {
				extra = 1;
				valid = (c >= 0xC2);
			} else if ((c & 0xF0) == 0xE0) {
				extra = 2;
				valid = true;
			} else if ((c & 0xF8) == 0xF0) {
				extra = 3;
				valid = (c <= 0xF4);
			}

			if (valid) {
				if (i + extra >= input.size()) {
					valid = false;
				} else {
					for (size_t j = 1; j <= extra; ++j) {
						if (!isUtf8ContinuationByte(static_cast<unsigned char>(input[i + j]))) {
							valid = false;
							break;
						}
					}
				}
			}

			if (valid) {
				out.append(input, i, extra + 1);
				i += extra + 1;
			} else {
				out.push_back('?');
				++i;
			}
		}
		return out;
	}

	std::string trimContextText(const std::string& text) {
		size_t start = 0;
		while (start < text.size() && isAsciiSpace(text[start])) start++;
		size_t end = text.size();
		while (end > start && isAsciiSpace(text[end - 1])) end--;
		return text.substr(start, end - start);
	}

	void appendSeparator(std::string& out) {
		if (!out.empty() && !isAsciiSpace(out.back())) out += ' ';
	}

	std::string cqParam(const std::string& segment, const std::string& key) {
		std::string needle = key + "=";
		size_t pos = segment.find(needle);
		if (pos == std::string::npos) return "";
		pos += needle.size();
		size_t end = segment.find_first_of(",]", pos);
		if (end == std::string::npos) end = segment.size();
		return segment.substr(pos, end - pos);
	}

	bool isMediaCqType(const std::string& type) {
		return type == "image" || type == "mface" || type == "marketface"
			|| type == "record" || type == "video" || type == "file"
			|| type == "json" || type == "xml" || type == "forward"
			|| type == "node" || type == "music" || type == "share";
	}

	std::string sanitizeContextContent(const std::string& content) {
		std::string out;
		for (size_t i = 0; i < content.size();) {
			if (content.compare(i, 4, "[CQ:") == 0) {
				size_t end = content.find(']', i + 4);
				if (end == std::string::npos) {
					out += content[i++];
					continue;
				}

				size_t typeStart = i + 4;
				size_t typeEnd = content.find_first_of(",]", typeStart);
				if (typeEnd == std::string::npos || typeEnd > end) typeEnd = end;
				std::string type = content.substr(typeStart, typeEnd - typeStart);
				std::string segment = content.substr(i, end - i + 1);

				if (type == "at") {
					std::string qq = cqParam(segment, "qq");
					appendSeparator(out);
					out += qq == "all" ? "@全体成员" : (qq.empty() ? "@某人" : "@" + qq);
				}
				else if (type == "face") {
					appendSeparator(out);
					out += "[表情]";
				}
				else if (isMediaCqType(type)) {
					appendSeparator(out);
				}
				i = end + 1;
				continue;
			}
			out += content[i++];
		}
		return sanitizeUtf8Text(trimContextText(out));
	}
}

Memory& Memory::get() { static Memory m; return m; }

bool Memory::open(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), (sqlite3**)&_db);
    if (rc) { std::cerr << "[Memory] 打开失败: " << sqlite3_errmsg((sqlite3*)_db) << std::endl; return false; }
    auto exec_or_fail = [&](const char* sql) -> bool {
        char* err = nullptr;
        int exec_rc = sqlite3_exec((sqlite3*)_db, sql, nullptr, nullptr, &err);
        if (exec_rc != SQLITE_OK) {
            std::cerr << "[Memory] 初始化失败: " << (err ? err : sqlite3_errmsg((sqlite3*)_db)) << std::endl;
            if (err) sqlite3_free(err);
            return false;
        }
        return true;
    };
    if (!exec_or_fail(
        "CREATE TABLE IF NOT EXISTS messages ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts INTEGER NOT NULL,"
        " chat_type TEXT NOT NULL,"
        " chat_id INTEGER NOT NULL,"
        " sender_id INTEGER NOT NULL,"
        " sender_name TEXT DEFAULT '',"
        " content TEXT NOT NULL,"
        " is_bot INTEGER DEFAULT 0"
        ");")) {
        sqlite3_close((sqlite3*)_db);
        _db = nullptr;
        return false;
    }
    if (!exec_or_fail("CREATE INDEX IF NOT EXISTS idx_chat ON messages(chat_type, chat_id, ts);")) {
        sqlite3_close((sqlite3*)_db);
        _db = nullptr;
        return false;
    }
    if (!exec_or_fail("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;")) {
        sqlite3_close((sqlite3*)_db);
        _db = nullptr;
        return false;
    }
    std::cout << "[Memory] 已打开 " << path << std::endl;
    return true;
}

void Memory::close() {
	if (_db) { sqlite3_close((sqlite3*)_db); _db = nullptr; }
}

void Memory::store(int64_t chat_id, const std::string& chat_type,
	int64_t sender_id, const std::string& sender_name,
	const std::string& content, bool is_bot) {
	if (!_db) return;
	auto ts = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	std::string safeChatType = sanitizeUtf8Text(chat_type);
	std::string safeSenderName = sanitizeUtf8Text(sender_name);
	std::string safeContent = sanitizeUtf8Text(content);
	sqlite3_stmt* stmt;
	const char* sql = "INSERT INTO messages(ts,chat_type,chat_id,sender_id,sender_name,content,is_bot) VALUES(?,?,?,?,?,?,?)";
	if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
	sqlite3_bind_int64(stmt, 1, ts);
	sqlite3_bind_text(stmt, 2, safeChatType.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, chat_id);
	sqlite3_bind_int64(stmt, 4, sender_id);
	sqlite3_bind_text(stmt, 5, safeSenderName.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, safeContent.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, is_bot ? 1 : 0);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);

	if (++_storeCnt % 50 == 0) {
		sqlite3_exec((sqlite3*)_db, "DELETE FROM messages WHERE id NOT IN (SELECT id FROM messages ORDER BY ts DESC LIMIT 100000)", nullptr, nullptr, nullptr);
	}
}

std::string Memory::recent(int64_t chat_id, const std::string& chat_type, int limit) {
	if (!_db) return "";
	sqlite3_stmt* stmt;
	const char* sql = "SELECT sender_name,sender_id,content,is_bot,ts FROM messages "
		"WHERE chat_id=? AND chat_type=? ORDER BY ts DESC LIMIT ?";
	if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
	sqlite3_bind_int64(stmt, 1, chat_id);
	sqlite3_bind_text(stmt, 2, chat_type.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, limit);

	struct Row {
		std::string sender_name;
		int64_t sender_id = 0;
		std::string content;
		int is_bot = 0;
	};
	std::vector<Row> rows;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* name = (const char*)sqlite3_column_text(stmt, 0);
		int64_t sid = sqlite3_column_int64(stmt, 1);
		const char* content = (const char*)sqlite3_column_text(stmt, 2);
		int is_bot = sqlite3_column_int(stmt, 3);
		rows.push_back({ sanitizeUtf8Text(name ? name : "?"), sid, sanitizeUtf8Text(content ? content : ""), is_bot });
	}
	sqlite3_finalize(stmt);
	if (rows.empty()) return "";

	std::ostringstream oss;
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		if (it->is_bot)
			oss << "[bot → ]: " << it->content << "\n";
		else
			oss << "[" << it->sender_name << "(" << it->sender_id << ")]: " << it->content << "\n";
	}
	return oss.str();
}

std::string Memory::recentTextContext(int64_t chat_id, const std::string& chat_type,
	int limit, int64_t exclude_sender_id, const std::string& exclude_content) {
	if (!_db) return "";
	if (limit <= 0) return "";
	int scanLimit = (limit * 4 > limit + 8) ? limit * 4 : limit + 8;
	sqlite3_stmt* stmt;
	const char* sql = "SELECT sender_name,sender_id,content,is_bot,ts FROM messages "
		"WHERE chat_id=? AND chat_type=? ORDER BY ts DESC LIMIT ?";
	if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
	sqlite3_bind_int64(stmt, 1, chat_id);
	sqlite3_bind_text(stmt, 2, chat_type.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 3, scanLimit);

	struct Row {
		std::string sender_name;
		int64_t sender_id = 0;
		std::string content;
		int is_bot = 0;
	};
	std::vector<Row> rows;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* name = (const char*)sqlite3_column_text(stmt, 0);
		int64_t sid = sqlite3_column_int64(stmt, 1);
		const char* raw = (const char*)sqlite3_column_text(stmt, 2);
		int is_bot = sqlite3_column_int(stmt, 3);
		std::string content = raw ? raw : "";
		if (exclude_sender_id != 0 && sid == exclude_sender_id && content == exclude_content) continue;
		content = sanitizeContextContent(content);
		if (content.empty()) continue;
		rows.push_back({ sanitizeUtf8Text(name ? name : "?"), sid, content, is_bot });
		if ((int)rows.size() >= limit) break;
	}
	sqlite3_finalize(stmt);
	if (rows.empty()) return "";

	std::ostringstream oss;
	for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
		if (it->is_bot)
			oss << "[bot → ]: " << it->content << "\n";
		else
			oss << "[" << it->sender_name << "(" << it->sender_id << ")]: " << it->content << "\n";
	}
	return oss.str();
}

std::string Memory::query(int64_t chat_id, const std::string& chat_type,
	int count, int64_t from_ts, int64_t to_ts, const std::string& keyword) {
	if (!_db) return "";
	if (count > 200) count = 200;
	if (count <= 0) count = 50;

	sqlite3_stmt* stmt;
	bool hasKeyword = !keyword.empty();
	bool hasFrom = (from_ts > 0), hasTo = (to_ts > 0);

	std::string sql = "SELECT sender_name,sender_id,content,is_bot,ts FROM messages WHERE chat_id=? AND chat_type=?";
	if (hasFrom) sql += " AND ts>=?";
	if (hasTo)   sql += " AND ts<=?";
	if (hasKeyword) sql += " AND content LIKE ?";
	sql += " ORDER BY ts DESC LIMIT ?";

	if (sqlite3_prepare_v2((sqlite3*)_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return "";
	int bi = 1;
	sqlite3_bind_int64(stmt, bi++, chat_id);
	sqlite3_bind_text(stmt, bi++, chat_type.c_str(), -1, SQLITE_TRANSIENT);
	if (hasFrom)    sqlite3_bind_int64(stmt, bi++, from_ts);
	if (hasTo)      sqlite3_bind_int64(stmt, bi++, to_ts);
	if (hasKeyword) { std::string like = "%" + keyword + "%"; sqlite3_bind_text(stmt, bi++, like.c_str(), -1, SQLITE_TRANSIENT); }
	sqlite3_bind_int(stmt, bi, count);

	std::ostringstream oss;
	int n = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* name = (const char*)sqlite3_column_text(stmt, 0);
		int64_t sid = sqlite3_column_int64(stmt, 1);
		const char* content = (const char*)sqlite3_column_text(stmt, 2);
		int is_bot = sqlite3_column_int(stmt, 3);
		std::string safeName = sanitizeUtf8Text(name ? name : "?");
		std::string safeContent = sanitizeUtf8Text(content ? content : "");
		if (is_bot)
			oss << "[bot → ]: " << safeContent << "\n";
		else
			oss << "[" << safeName << "(" << sid << ")]: " << safeContent << "\n";
		n++;
	}
	sqlite3_finalize(stmt);
	return n == 0 ? "暂无匹配的历史消息" : oss.str();
}

std::string Memory::search(const std::string& keyword, int64_t chat_id,
	const std::string& chat_type, int limit) {
	if (!_db || keyword.empty()) return "";
	sqlite3_stmt* stmt;
	std::string sql;
	if (chat_id > 0 && !chat_type.empty()) {
		sql = "SELECT sender_name,sender_id,content,is_bot,ts FROM messages "
			"WHERE content LIKE ? AND chat_id=? AND chat_type=? ORDER BY ts DESC LIMIT ?";
	} else {
		sql = "SELECT sender_name,sender_id,content,is_bot,chat_type,chat_id,ts FROM messages "
			"WHERE content LIKE ? ORDER BY ts DESC LIMIT ?";
	}
	if (sqlite3_prepare_v2((sqlite3*)_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return "";
	std::string like = "%" + keyword + "%";
	sqlite3_bind_text(stmt, 1, like.c_str(), -1, SQLITE_TRANSIENT);
	if (chat_id > 0 && !chat_type.empty()) {
		sqlite3_bind_int64(stmt, 2, chat_id);
		sqlite3_bind_text(stmt, 3, chat_type.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(stmt, 4, limit);
	} else {
		sqlite3_bind_int(stmt, 2, limit);
	}

	std::ostringstream oss;
	int n = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* name = (const char*)sqlite3_column_text(stmt, 0);
		int64_t sid = sqlite3_column_int64(stmt, 1);
		const char* content = (const char*)sqlite3_column_text(stmt, 2);
		int is_bot = sqlite3_column_int(stmt, 3);
		if (chat_id == 0) {
			const char* ct = (const char*)sqlite3_column_text(stmt, 4);
			int64_t cid = sqlite3_column_int64(stmt, 5);
			oss << "[" << (ct ? ct : "") << ":" << cid << "] ";
		}
			std::string safeName = sanitizeUtf8Text(name ? name : "?");
			std::string safeContent = sanitizeUtf8Text(content ? content : "");
			if (is_bot)
				oss << "[bot → ]: " << safeContent << "\n";
			else
				oss << "[" << safeName << "(" << sid << ")]: " << safeContent << "\n";
		n++;
	}
	sqlite3_finalize(stmt);
	return n == 0 ? "未找到包含 '" + keyword + "' 的历史消息" : oss.str();
}

std::string Memory::recentByDays(int64_t chat_id, const std::string& chat_type,
	int days, int max_messages)
{
	if (!_db || days <= 0) return "";
	if (max_messages <= 0) max_messages = 200;
	if (max_messages > 500) max_messages = 500;

	int64_t now_ts = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	int64_t from_ts = now_ts - ((int64_t)days * 86400);

	sqlite3_stmt* stmt;
	const char* sql = "SELECT sender_name,sender_id,content,is_bot,ts FROM messages "
		"WHERE chat_id=? AND chat_type=? AND ts>=? ORDER BY ts ASC LIMIT ?";

	if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";

	sqlite3_bind_int64(stmt, 1, chat_id);
	sqlite3_bind_text(stmt, 2, chat_type.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_int64(stmt, 3, from_ts);
	sqlite3_bind_int(stmt, 4, max_messages);

	std::ostringstream oss;
	int n = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char* name = (const char*)sqlite3_column_text(stmt, 0);
		int64_t sid = sqlite3_column_int64(stmt, 1);
		const char* content = (const char*)sqlite3_column_text(stmt, 2);
		int is_bot = sqlite3_column_int(stmt, 3);
		std::string safeName = sanitizeUtf8Text(name ? name : "?");
		std::string safeContent = sanitizeUtf8Text(content ? content : "");
		if (is_bot)
			oss << "[bot → ]: " << safeContent << "\n";
		else
			oss << "[" << safeName << "(" << sid << ")]: " << safeContent << "\n";
		n++;
	}
	sqlite3_finalize(stmt);
	return n == 0 ? "" : oss.str();
}

std::string Memory::getGroupStyleQuery(int64_t group_id, int days) {
	// 复用 recentByDays，取群消息返回分析友好格式
	return recentByDays(group_id, "group", days, 200);
}
