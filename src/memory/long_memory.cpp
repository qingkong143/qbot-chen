#include "src/memory/long_memory.h"
#include "src/core/config.h"
#include "src/bot/models.h"
#include <sqlite3.h>

namespace {
double estimateImportance(const LongMemoryContext& ctx) {
    std::string text = LongMemory::trim(ctx.user_message + " " + ctx.bot_reply);
    if (text.size() < 12) return 0.0;

    double score = 0.35;
    if (text.size() >= 40) score += 0.10;
    if (text.size() >= 80) score += 0.10;
    if (text.size() >= 160) score += 0.05;

    static const std::vector<std::string> strongMarkers = {
        "记住", "别忘", "以后", "喜欢", "不喜欢", "讨厌", "我叫", "我是",
        "生日", "工作", "学校", "住", "偏好", "纠正", "不是", "我没说过",
    };
    for (const auto& marker : strongMarkers) {
        if (text.find(marker) != std::string::npos) {
            score += 0.12;
        }
    }

    if (!ctx.bot_reply.empty()) {
        score += 0.05;
    }

    if (score > 1.0) score = 1.0;
    if (score < 0.0) score = 0.0;
    return score;
}
} // namespace

LongMemory& LongMemory::get() {
    static LongMemory instance;
    return instance;
}

bool LongMemory::open(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), (sqlite3**)&_db);
    if (rc) {
        std::cerr << "[长期记忆] 打开失败: " << sqlite3_errmsg((sqlite3*)_db) << std::endl;
        return false;
    }
    ensureSchema();
    std::cout << "[长期记忆] 已打开 " << path << std::endl;
    return true;
}

void LongMemory::close() {
    if (_db) {
        sqlite3_close((sqlite3*)_db);
        _db = nullptr;
    }
}

void LongMemory::ensureSchema() {
    if (!_db) return;
    const char* sql =
        "CREATE TABLE IF NOT EXISTS person_profiles ("
        " user_id INTEGER NOT NULL,"
        " chat_id INTEGER NOT NULL,"
        " chat_type TEXT NOT NULL,"
        " profile_json TEXT NOT NULL,"
        " updated_ts INTEGER NOT NULL,"
        " PRIMARY KEY(user_id, chat_id, chat_type)"
        ");"
        "CREATE TABLE IF NOT EXISTS person_facts ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " user_id INTEGER NOT NULL,"
        " chat_id INTEGER NOT NULL,"
        " chat_type TEXT NOT NULL,"
        " fact TEXT NOT NULL,"
        " category TEXT NOT NULL,"
        " confidence REAL NOT NULL,"
        " source_id INTEGER DEFAULT 0,"
        " updated_ts INTEGER NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active'"
        ");"
        "CREATE TABLE IF NOT EXISTS episodes ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " chat_id INTEGER NOT NULL,"
        " chat_type TEXT NOT NULL,"
        " title TEXT NOT NULL,"
        " summary TEXT NOT NULL,"
        " start_ts INTEGER NOT NULL,"
        " end_ts INTEGER NOT NULL,"
        " importance REAL NOT NULL,"
        " source_id INTEGER DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_sources ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " source_type TEXT NOT NULL,"
        " chat_id INTEGER NOT NULL,"
        " chat_type TEXT NOT NULL,"
        " ref TEXT DEFAULT '',"
        " created_ts INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_edges ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " from_type TEXT NOT NULL,"
        " from_id INTEGER NOT NULL,"
        " relation TEXT NOT NULL,"
        " to_type TEXT NOT NULL,"
        " to_id INTEGER NOT NULL,"
        " weight REAL NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_feedback ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " memory_type TEXT NOT NULL,"
        " memory_id INTEGER NOT NULL,"
        " feedback TEXT NOT NULL,"
        " action TEXT NOT NULL,"
        " created_ts INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_embeddings ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " memory_type TEXT NOT NULL,"
        " memory_id INTEGER NOT NULL,"
        " text TEXT NOT NULL,"
        " vector_json TEXT DEFAULT '',"
        " model TEXT DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'pending',"
        " updated_ts INTEGER NOT NULL,"
        " UNIQUE(memory_type, memory_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_facts_lookup ON person_facts(chat_type, chat_id, user_id, status, updated_ts);"
        "CREATE INDEX IF NOT EXISTS idx_episodes_lookup ON episodes(chat_type, chat_id, end_ts);"
        "CREATE INDEX IF NOT EXISTS idx_embeddings_status ON memory_embeddings(status, updated_ts);";
    char* err = nullptr;
    if (sqlite3_exec((sqlite3*)_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[长期记忆] 建表失败: " << (err ? err : "unknown") << std::endl;
        sqlite3_free(err);
    }
}

std::string LongMemory::trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string LongMemory::clip(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    size_t end = maxLen;
    while (end > 0 && (static_cast<unsigned char>(s[end - 1]) & 0xC0) == 0x80) end--;
    return s.substr(0, end) + "...";
}

bool LongMemory::shouldRememberMessage(const std::string& text) const {
    std::string t = trim(sanitizeMessageForMemory(text));
    if (t.size() < 12) return false;
    static const std::vector<std::string> noise = {"哈哈", "草", "艹", "笑死", "？", "?", "。", "哦", "嗯", "好"};
    for (const auto& n : noise) {
        if (t == n) return false;
    }
    return true;
}

std::string LongMemory::sanitizeMessageForMemory(const std::string& text) const {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 4, "[CQ:") == 0) {
            size_t end = text.find(']', i + 4);
            if (end == std::string::npos) {
                out += text[i++];
                continue;
            }

            size_t typeStart = i + 4;
            size_t typeEnd = text.find_first_of(",]", typeStart);
            if (typeEnd == std::string::npos || typeEnd > end) typeEnd = end;
            std::string type = text.substr(typeStart, typeEnd - typeStart);
            if (type == "face") {
                out += "[表情]";
            }
            else if (type != "at") {
                out += " ";
            }
            i = end + 1;
            continue;
        }
        out += text[i++];
    }
    return trim(out);
}

std::string LongMemory::buildPromptContext(const LongMemoryContext& ctx) {
    const auto& cfg = Config::get().a_memorix();
    if (!_db || !cfg.enabled || !cfg.enable_query) return "";

    std::string out;
    if (cfg.inject_profile) {
        std::string profile = queryProfile(ctx, cfg.max_profile_chars);
        if (!profile.empty()) out += "[人物画像]\n" + profile + "\n";
    }
    std::string facts = hybridQueryFacts(ctx, cfg.max_facts);
    if (!facts.empty()) out += "[相关长期记忆]\n" + facts + "\n";
    std::string relations = queryGraphRelations(ctx.user_id);
    if (!relations.empty()) out += "[图谱关系]\n" + relations + "\n";
    std::string episodes = queryEpisodes(ctx, cfg.max_episodes);
    if (!episodes.empty()) out += "[相关经历片段]\n" + episodes + "\n";

    if (!out.empty()) std::cout << "[长期记忆] 已注入 " << out.size() << " 字符（混合检索）" << std::endl;
    return out;
}

std::string LongMemory::queryProfile(const LongMemoryContext& ctx, int maxChars) {
    if (!_db || ctx.user_id == 0 || maxChars == 0) return "";
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT profile_json FROM person_profiles WHERE user_id=? AND chat_id=? AND chat_type=? LIMIT 1";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_int64(stmt, 1, ctx.user_id);
    sqlite3_bind_int64(stmt, 2, ctx.chat_id);
    sqlite3_bind_text(stmt, 3, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(stmt, 0);
        result = text ? text : "";
    }
    sqlite3_finalize(stmt);
    return maxChars > 0 ? clip(result, static_cast<size_t>(maxChars)) : result;
}

std::string LongMemory::queryFacts(const LongMemoryContext& ctx, int limit) {
    if (!_db || limit <= 0) return "";
    sqlite3_stmt* stmt = nullptr;
    // 分两段查询避免 OR 导致索引失效：先查用户专属(fact=user_id)，再查群通用(fact=user_id=0)
    const char* sql_personal = "SELECT fact,category,confidence FROM person_facts WHERE chat_id=? AND chat_type=? AND user_id=? AND status='active' ORDER BY confidence DESC, updated_ts DESC LIMIT ?";
    const char* sql_group = "SELECT fact,category,confidence FROM person_facts WHERE chat_id=? AND chat_type=? AND user_id=0 AND status='active' ORDER BY confidence DESC, updated_ts DESC LIMIT ?";

    std::ostringstream oss;
    int remaining = limit;

    // 个人事实：占用一半配额
    int personalLimit = remaining / 2;
    if (personalLimit <= 0) personalLimit = 1;
    stmt = nullptr;
    if (sqlite3_prepare_v2((sqlite3*)_db, sql_personal, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, ctx.chat_id);
        sqlite3_bind_text(stmt, 2, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, ctx.user_id);
        sqlite3_bind_int(stmt, 4, personalLimit);
        while (remaining > 0 && sqlite3_step(stmt) == SQLITE_ROW) {
            const char* fact = (const char*)sqlite3_column_text(stmt, 0);
            const char* category = (const char*)sqlite3_column_text(stmt, 1);
            double confidence = sqlite3_column_double(stmt, 2);
            oss << "- " << (category ? category : "fact") << ": " << (fact ? fact : "") << " (" << confidence << ")\n";
            remaining--;
        }
        sqlite3_finalize(stmt);
    }

    // 群通用事实：剩余配额
    if (remaining > 0) {
        int groupLimit = remaining;
        stmt = nullptr;
        if (sqlite3_prepare_v2((sqlite3*)_db, sql_group, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, ctx.chat_id);
            sqlite3_bind_text(stmt, 2, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, groupLimit);
            while (remaining > 0 && sqlite3_step(stmt) == SQLITE_ROW) {
                const char* fact = (const char*)sqlite3_column_text(stmt, 0);
                const char* category = (const char*)sqlite3_column_text(stmt, 1);
                double confidence = sqlite3_column_double(stmt, 2);
                oss << "- " << (category ? category : "fact") << ": " << (fact ? fact : "") << " (" << confidence << ")\n";
                remaining--;
            }
            sqlite3_finalize(stmt);
        }
    }

    return oss.str();
}

std::string LongMemory::queryEpisodes(const LongMemoryContext& ctx, int limit) {
    if (!_db || limit <= 0) return "";
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT title,summary FROM episodes WHERE chat_id=? AND chat_type=? ORDER BY importance DESC, end_ts DESC LIMIT ?";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return "";
    sqlite3_bind_int64(stmt, 1, ctx.chat_id);
    sqlite3_bind_text(stmt, 2, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);
    std::ostringstream oss;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* title = (const char*)sqlite3_column_text(stmt, 0);
        const char* summary = (const char*)sqlite3_column_text(stmt, 1);
        oss << "- " << (title ? title : "片段") << ": " << (summary ? summary : "") << "\n";
    }
    sqlite3_finalize(stmt);
    return oss.str();
}

void LongMemory::writeBackAfterReply(const LongMemoryContext& ctx) {
    const auto& cfg = Config::get().a_memorix();
    if (!_db || !cfg.enabled || !cfg.after_reply) return;

    LongMemoryContext cleanCtx = ctx;
    cleanCtx.chat_type = sanitizeMessageForMemory(cleanCtx.chat_type);
    cleanCtx.user_name = sanitizeMessageForMemory(cleanCtx.user_name);
    cleanCtx.user_message = sanitizeMessageForMemory(cleanCtx.user_message);
    cleanCtx.bot_reply = sanitizeMessageForMemory(cleanCtx.bot_reply);

    if (!shouldRememberMessage(cleanCtx.user_message)) return;

    const double importance = estimateImportance(cleanCtx);
    if (importance < cfg.min_importance) {
        std::cout << "[长期记忆] 重要度不足，跳过写回 (" << importance << ")" << std::endl;
        return;
    }

    if (cfg.write_person_facts) {
        upsertProfile(cleanCtx);
        std::string fact = cleanCtx.user_name + " 最近提到：" + clip(trim(cleanCtx.user_message), 120);
        if (!factExists(cleanCtx, fact))
            insertFact(cleanCtx, fact, "message", 0.7);
    }
    if (cfg.write_chat_summary) {
        insertEpisode(cleanCtx, "对话片段", cleanCtx.user_name + " 说：" + clip(trim(cleanCtx.user_message), 120) + "；机器人回复：" + clip(trim(cleanCtx.bot_reply), 160), 0.7);
    }
    std::cout << "[长期记忆] 已写回候选记忆" << std::endl;
}

void LongMemory::upsertProfile(const LongMemoryContext& ctx) {
    if (!_db || ctx.user_id == 0) return;
    int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    LongMemoryContext cleanCtx = ctx;
    cleanCtx.chat_type = sanitizeMessageForMemory(cleanCtx.chat_type);
    cleanCtx.user_name = sanitizeMessageForMemory(cleanCtx.user_name);
    cleanCtx.user_message = sanitizeMessageForMemory(cleanCtx.user_message);

    json profile = {
        {"user_id", cleanCtx.user_id},
        {"name", cleanCtx.user_name},
        {"last_seen", now},
        {"last_message", clip(trim(cleanCtx.user_message), 120)}
    };
    std::string text = profile.dump();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO person_profiles(user_id,chat_id,chat_type,profile_json,updated_ts) VALUES(?,?,?,?,?) ON CONFLICT(user_id,chat_id,chat_type) DO UPDATE SET profile_json=excluded.profile_json, updated_ts=excluded.updated_ts";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, cleanCtx.user_id);
    sqlite3_bind_int64(stmt, 2, cleanCtx.chat_id);
    sqlite3_bind_text(stmt, 3, cleanCtx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

bool LongMemory::factExists(const LongMemoryContext& ctx, const std::string& fact) const {
    if (!_db || fact.empty()) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1 FROM person_facts WHERE user_id=? AND chat_id=? AND chat_type=? AND fact=? AND status='active' LIMIT 1";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, ctx.user_id);
    sqlite3_bind_int64(stmt, 2, ctx.chat_id);
    sqlite3_bind_text(stmt, 3, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, fact.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
}

void LongMemory::insertFact(const LongMemoryContext& ctx, const std::string& fact, const std::string& category, double confidence) {
    if (!_db || fact.empty()) return;
    int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO person_facts(user_id,chat_id,chat_type,fact,category,confidence,updated_ts,status) VALUES(?,?,?,?,?,?,?,'active')";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, ctx.user_id);
    sqlite3_bind_int64(stmt, 2, ctx.chat_id);
    sqlite3_bind_text(stmt, 3, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, fact.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 6, confidence);
    sqlite3_bind_int64(stmt, 7, now);
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        int64_t id = sqlite3_last_insert_rowid((sqlite3*)_db);
        sqlite3_finalize(stmt);
        enqueueEmbedding("fact", id, fact);
        return;
    }
    sqlite3_finalize(stmt);
}

double LongMemory::simpleTokenSimilarity(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    std::vector<std::string> tokensA, tokensB;
    std::istringstream issA(a), issB(b);
    std::string token;
    while (issA >> token) tokensA.push_back(token);
    while (issB >> token) tokensB.push_back(token);
    int match = 0;
    for (const auto& tA : tokensA) {
        for (const auto& tB : tokensB) {
            if (tA == tB) { match++; break; }
        }
    }
    int total = tokensA.size() > tokensB.size() ? (int)tokensA.size() : (int)tokensB.size();
    return total > 0 ? (double)match / total : 0.0;
}

std::vector<std::pair<int64_t, double>> LongMemory::keywordSearchFacts(const std::string& query, int limit) {
    std::vector<std::pair<int64_t, double>> results;
    if (!_db || query.empty() || limit <= 0) return results;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, fact FROM person_facts WHERE status='active' ORDER BY updated_ts DESC LIMIT 100";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return results;
    std::vector<std::pair<int64_t, double>> candidates;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        const char* fact = (const char*)sqlite3_column_text(stmt, 1);
        if (fact) {
            double score = simpleTokenSimilarity(query, fact);
            if (score > 0.1) candidates.push_back({id, score});
        }
    }
    sqlite3_finalize(stmt);
    std::sort(candidates.begin(), candidates.end(),
        [](const std::pair<int64_t, double>& a, const std::pair<int64_t, double>& b) { return a.second > b.second; });
    for (int i = 0; i < (candidates.size() < limit ? (int)candidates.size() : limit); i++) {
        results.push_back(candidates[i]);
    }
    return results;
}

std::string LongMemory::hybridQueryFacts(const LongMemoryContext& ctx, int limit) {
    if (!_db || limit <= 0) return "";
    std::ostringstream oss;
    std::map<int64_t, double> scoreMap;
    auto keywordResults = keywordSearchFacts(ctx.user_message, limit);
    for (const auto& kw : keywordResults) {
        scoreMap[kw.first] += kw.second * 0.6;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, fact, category, confidence FROM person_facts WHERE chat_id=? AND chat_type=? AND user_id=? AND status='active' ORDER BY confidence DESC LIMIT ?";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, ctx.chat_id);
        sqlite3_bind_text(stmt, 2, ctx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, ctx.user_id);
        sqlite3_bind_int(stmt, 4, limit);
        int rank = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && rank < limit) {
            int64_t id = sqlite3_column_int64(stmt, 0);
            scoreMap[id] += (1.0 - rank * 0.1) * 0.2;
            rank++;
        }
        sqlite3_finalize(stmt);
    }
    std::vector<std::pair<int64_t, double>> merged(scoreMap.begin(), scoreMap.end());
    std::sort(merged.begin(), merged.end(),
        [](const std::pair<int64_t, double>& a, const std::pair<int64_t, double>& b) { return a.second > b.second; });
    sql = "SELECT fact, category FROM person_facts WHERE id=? LIMIT 1";
    for (int i = 0; i < (merged.size() < limit ? (int)merged.size() : limit); i++) {
        if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, merged[i].first);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* fact = (const char*)sqlite3_column_text(stmt, 0);
                const char* category = (const char*)sqlite3_column_text(stmt, 1);
                oss << "- " << (category ? category : "记忆") << ": " << (fact ? fact : "") << "\n";
            }
            sqlite3_finalize(stmt);
        }
    }
    return oss.str();
}

void LongMemory::insertRelationEdge(int64_t userId1, int64_t userId2, const std::string& relation) {
    if (!_db || userId1 <= 0 || userId2 <= 0) return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO memory_edges(from_type,from_id,relation,to_type,to_id,weight) VALUES('user',?,?,'user',?,1.0)";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, userId1);
        sqlite3_bind_text(stmt, 2, relation.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, userId2);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::string LongMemory::queryGraphRelations(int64_t userId) {
    std::ostringstream oss;
    if (!_db || userId <= 0) return "";
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT relation, to_id FROM memory_edges WHERE from_id=? ORDER BY weight DESC LIMIT 5";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, userId);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* rel = (const char*)sqlite3_column_text(stmt, 0);
            int64_t toId = sqlite3_column_int64(stmt, 1);
            oss << "- " << (rel ? rel : "关系") << ": 用户 " << toId << "\n";
        }
        sqlite3_finalize(stmt);
    }
    return oss.str();
}

void LongMemory::insertEpisode(const LongMemoryContext& ctx, const std::string& title, const std::string& summary, double importance) {
    if (!_db || summary.empty()) return;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    LongMemoryContext cleanCtx = ctx;
    cleanCtx.chat_type = sanitizeMessageForMemory(cleanCtx.chat_type);
    std::string cleanTitle = sanitizeMessageForMemory(title);
    std::string cleanSummary = sanitizeMessageForMemory(summary);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO episodes(chat_id,chat_type,title,summary,start_ts,end_ts,importance,source_id) "
        "VALUES(?,?,?,?,?,?,?,0)";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, cleanCtx.chat_id);
        sqlite3_bind_text(stmt, 2, cleanCtx.chat_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, cleanTitle.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, cleanSummary.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, now);
        sqlite3_bind_int64(stmt, 6, now);
        sqlite3_bind_double(stmt, 7, importance);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        std::cout << "[长期记忆] 已记录剧集: " << cleanTitle << std::endl;
    }
}

void LongMemory::recordFeedback(int64_t factId, const std::string& feedback, const std::string& action) {
    if (!_db || factId <= 0) return;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO memory_feedback(memory_type,memory_id,feedback,action,created_ts) "
        "VALUES('fact',?,?,?,?)";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, factId);
        sqlite3_bind_text(stmt, 2, feedback.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, action.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            if (action == "outdated" || action == "corrected") {
                const char* updateSql = "UPDATE person_facts SET status=? WHERE id=?";
                sqlite3_stmt* updateStmt = nullptr;
                if (sqlite3_prepare_v2((sqlite3*)_db, updateSql, -1, &updateStmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(updateStmt, 1, action.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(updateStmt, 2, factId);
                    sqlite3_step(updateStmt);
                    sqlite3_finalize(updateStmt);
                    std::cout << "[长期记忆] 反馈已记录: " << action << std::endl;
                }
            }
        }
        sqlite3_finalize(stmt);
    }
}

void LongMemory::enqueueEmbedding(const std::string& memoryType, int64_t memoryId, const std::string& text) {
    if (!_db || memoryType.empty() || memoryId <= 0) return;
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO memory_embeddings(memory_type,memory_id,text,status,updated_ts) VALUES(?,?,?,'pending',?) ON CONFLICT(memory_type,memory_id) DO UPDATE SET updated_ts=excluded.updated_ts";
    if (sqlite3_prepare_v2((sqlite3*)_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, memoryType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, memoryId);
        sqlite3_bind_text(stmt, 3, text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// 余弦相似度计算
static double cosineSimilarity(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;

    double dotProduct = 0.0, normA = 0.0, normB = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dotProduct += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    if (normA == 0.0 || normB == 0.0) return 0.0;
    return dotProduct / (std::sqrt(normA) * std::sqrt(normB));
}

// 向量检索：通过 embedding API 找相似的 facts
std::vector<std::pair<int64_t, double>> LongMemory::vectorSearchFacts(
    const std::string& query, int limit, class Models& models, CURL* curl) {
    std::vector<std::pair<int64_t, double>> results;
    if (!_db || query.empty()) return results;

    // 1. 获取查询向量
    auto queryVector = models.getEmbedding(curl, query);
    if (queryVector.empty()) {
        std::cerr << "[向量检索] 获取查询向量失败，降级到关键词检索" << std::endl;
        return keywordSearchFacts(query, limit);
    }

    // 2. 从 SQLite 获取所有已向量化的 facts
    sqlite3_stmt* stmt = nullptr;
    const char* searchSql =
        "SELECT pf.id, me.embedding_json FROM person_facts pf "
        "LEFT JOIN memory_embeddings me ON me.memory_type='fact' AND me.memory_id=pf.id "
        "WHERE me.embedding_json IS NOT NULL AND me.embedding_json != '' "
        "LIMIT 1000";

    if (sqlite3_prepare_v2((sqlite3*)_db, searchSql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[向量检索] SQL 准备失败" << std::endl;
        return keywordSearchFacts(query, limit);
    }

    // 3. 计算相似度并排序
    std::map<int64_t, double> scoreMap;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t factId = sqlite3_column_int64(stmt, 0);
        const char* embeddingJsonStr = (const char*)sqlite3_column_text(stmt, 1);

        if (!embeddingJsonStr) continue;

        try {
            json embeddingJson = json::parse(embeddingJsonStr);
            if (embeddingJson.is_array()) {
                std::vector<double> factVector;
                for (const auto& val : embeddingJson) {
                    factVector.push_back(val.get<double>());
                }

                double similarity = cosineSimilarity(queryVector, factVector);
                if (similarity > 0.3) {  // 阈值：0.3
                    scoreMap[factId] = similarity;
                }
            }
        } catch (...) {
            // 解析失败，跳过此记录
        }
    }
    sqlite3_finalize(stmt);

    // 4. 按相似度排序
    for (const auto& [factId, score] : scoreMap) {
        results.push_back({factId, score});
        if ((int)results.size() >= limit) break;
    }

    std::sort(results.begin(), results.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    return results;
}
