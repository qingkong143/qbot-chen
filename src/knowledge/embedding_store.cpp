#include "src/knowledge/embedding_store.h"
#include "src/knowledge/embedding_service.h"
#include "src/infra/logger.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <openssl/sha.h>
#endif

namespace fs = std::filesystem;

namespace {
std::vector<unsigned char> encodeFloatVector(const std::vector<float>& vec) {
    std::vector<unsigned char> blob(vec.size() * sizeof(float));
    if (!vec.empty()) {
        std::memcpy(blob.data(), vec.data(), blob.size());
    }
    return blob;
}

std::vector<float> decodeFloatVector(const void* data, int bytes) {
    std::vector<float> vec;
    if (data == nullptr || bytes <= 0 || bytes % static_cast<int>(sizeof(float)) != 0) {
        return vec;
    }

    const auto* ptr = static_cast<const float*>(data);
    vec.assign(ptr, ptr + (bytes / static_cast<int>(sizeof(float))));
    return vec;
}

bool exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        Logger::get().warn("[嵌入库]", std::string("SQLite 执行失败: ") + (err ? err : sqlite3_errmsg(db)));
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}
}

std::string EmbeddingStore::_compute_hash(const std::string& namespace_name, const std::string& text) {
    std::string combined = namespace_name + "|" + text;
    uint64_t hash = 14695981039346656037ULL;
    for (char c : combined) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)hash);
    return std::string(buf);
}

float EmbeddingStore::_cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;

    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    norm_a = std::sqrt(norm_a);
    norm_b = std::sqrt(norm_b);

    if (norm_a < 1e-6f || norm_b < 1e-6f) return 0.0f;
    return dot / (norm_a * norm_b);
}

void EmbeddingStore::_normalize_l2(std::vector<float>& vec) {
    float norm = 0.0f;
    for (float v : vec) {
        norm += v * v;
    }
    norm = std::sqrt(norm);
    if (norm < 1e-6f) return;
    for (float& v : vec) {
        v /= norm;
    }
}

EmbeddingStore::EmbeddingStore(const std::string& namespace_name, const std::string& dir_path)
    : _namespace(namespace_name), _dir_path(dir_path) {
    _data_file = dir_path + "/" + namespace_name + ".json";
    _db_file = dir_path + "/" + namespace_name + ".db";
    _index_file = dir_path + "/" + namespace_name + ".index";
    _idx2hash_file = dir_path + "/" + namespace_name + "_i2h.json";
    _test_vectors_file = dir_path + "/" + namespace_name + "_test_vectors.json";
    Logger::get().info("[嵌入库]", "初始化: namespace=" + namespace_name + ", db=" + _db_file);
}

EmbeddingStore::~EmbeddingStore() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_faiss_index != nullptr) {
        delete static_cast<int*>(_faiss_index);
        _faiss_index = nullptr;
    }
}

void EmbeddingStore::batch_insert_strs(const std::vector<std::string>& strs) {
    if (strs.empty()) return;

    Logger::get().info("[嵌入库]", "批量插入 " + std::to_string(strs.size()) + " 条记录");

    auto& client = EmbeddingServiceClient::get();
    auto results = client.embed_texts_sync(strs, 5);
    auto now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;

    int inserted = 0, skipped = 0;
    std::vector<EmbeddingStoreItem> upserts;
    bool should_save = false;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (size_t i = 0; i < strs.size() && i < results.size(); i++) {
            const auto& text = strs[i];
            const auto& result = results[i];

            if (!result.success || result.embedding.empty()) {
                Logger::get().warn("[嵌入库]", "获取向量失败: " + text.substr(0, 50));
                skipped++;
                continue;
            }

            std::string hash = _compute_hash(_namespace, text);
            auto it = _store.find(hash);
            if (it != _store.end()) {
                it->second.frequency++;
                upserts.push_back(it->second);
                skipped++;
                should_save = true;
                continue;
            }

            EmbeddingStoreItem item;
            item.hash = hash;
            item.embedding = result.embedding;
            item.content = text;
            item.timestamp = now;
            item.frequency = 1;

            _store[hash] = item;
            upserts.push_back(item);
            inserted++;
            should_save = true;
        }

        if (_max_size > 0 && _store.size() > _max_size) {
            _evict_oldest();
            should_save = true;
        }
    }

    if (should_save) {
        save_to_file();
    }

    Logger::get().info("[嵌入库]", "插入完成: 新增=" + std::to_string(inserted) + ", 跳过=" + std::to_string(skipped));
}

std::vector<std::pair<std::string, float>> EmbeddingStore::search_top_k(
    const std::vector<float>& query, int k) {

    std::vector<std::pair<std::string, float>> results;

    if (query.empty() || k <= 0) return results;

    std::lock_guard<std::mutex> lock(_mutex);

    if (_store.empty()) {
        Logger::get().warn("[嵌入库]", "库为空，无法搜索");
        return results;
    }

    if (_store.size() > FAISS_THRESHOLD && _faiss_index != nullptr) {
        return _search_with_faiss(query, k);
    }
    return _search_linear(query, k);
}

std::vector<std::pair<std::string, float>> EmbeddingStore::_search_linear(
    const std::vector<float>& query, int k) {

    std::vector<std::pair<std::string, float>> results;

    size_t n = _store.size();
    if (n == 0) return results;

    // 预分配：用大值初始化，避免每个元素都 push_back
    std::vector<std::pair<float, std::string>> similarities;
    similarities.reserve(n);

    for (const auto& [hash, item] : _store) {
        float sim = _cosine_similarity(query, item.embedding);
        similarities.emplace_back(sim, hash);
    }

    // 部分排序：只把前 k 个最大元素排好序，O(n log k) 而非 O(n log n)
    int need = std::min(k, (int)n);
    std::partial_sort(similarities.begin(), similarities.begin() + need, similarities.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    for (int i = 0; i < need; i++) {
        results.push_back({similarities[i].second, similarities[i].first});
    }

    Logger::get().debug("[嵌入库]", "线性搜索完成: 返回 " + std::to_string(need) + " 条结果");
    return results;
}

std::vector<std::pair<std::string, float>> EmbeddingStore::_search_with_faiss(
    const std::vector<float>& query, int k) {

    Logger::get().debug("[嵌入库]", "库大小: " + std::to_string(_store.size()) +
                       " > " + std::to_string(FAISS_THRESHOLD) +
                       "，建议集成 Faiss 库以加速搜索");
    return _search_linear(query, k);
}

void EmbeddingStore::_build_faiss_index() {
    Logger::get().debug("[嵌入库]", "标记索引为脏，待 Faiss 库集成时重建");
    _dirty = false;
}

bool EmbeddingStore::needs_rebuild() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _dirty || (_store.size() > FAISS_THRESHOLD && _faiss_index == nullptr);
}

void EmbeddingStore::rebuild_faiss_index() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_store.size() > FAISS_THRESHOLD) {
        Logger::get().info("[嵌入库]", "重建 Faiss 索引（库大小: " + std::to_string(_store.size()) + "）");
        _build_faiss_index();
    } else {
        Logger::get().debug("[嵌入库]", "库大小 < " + std::to_string(FAISS_THRESHOLD) + "，无需 Faiss 索引");
    }
}

std::string EmbeddingStore::get_content(const std::string& hash) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _store.find(hash);
    if (it != _store.end()) {
        return it->second.content;
    }
    return "";
}

void EmbeddingStore::request_save() {
    _save_pending.store(true, std::memory_order_release);
    _last_save_request = std::chrono::steady_clock::now();
}

void EmbeddingStore::flush_pending_saves() {
    if (!_save_pending.load(std::memory_order_acquire)) return;

    auto now = std::chrono::steady_clock::now();
    if (_last_save_request.time_since_epoch().count() != 0 &&
        now - _last_save_request < SAVE_DEBOUNCE_WINDOW) {
        return;
    }

    _save_pending.store(false, std::memory_order_release);
    save_to_file();
}

void EmbeddingStore::save_to_file() {
    std::lock_guard<std::mutex> lock(_mutex);

    fs::create_directories(_dir_path);

    bool migrated = false;
    if (!fs::exists(_db_file) && fs::exists(_data_file)) {
        migrated = true;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(_db_file.c_str(), &db) != SQLITE_OK) {
        Logger::get().warn("[嵌入库]", "打开 SQLite 失败: " + std::string(sqlite3_errmsg(db)));
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_busy_timeout(db, 3000);
    exec_sql(db, "PRAGMA journal_mode=WAL;");
    exec_sql(db, "PRAGMA synchronous=NORMAL;");

    const char* create_sql =
        "CREATE TABLE IF NOT EXISTS embeddings ("
        "hash TEXT PRIMARY KEY,"
        "content TEXT NOT NULL,"
        "embedding BLOB NOT NULL,"
        "timestamp INTEGER NOT NULL,"
        "frequency INTEGER NOT NULL DEFAULT 1);"
        "CREATE TABLE IF NOT EXISTS meta ("
        "key TEXT PRIMARY KEY,"
        "value TEXT NOT NULL);";
    if (!exec_sql(db, create_sql)) {
        sqlite3_close(db);
        return;
    }

    if (migrated) {
        try {
            std::ifstream file(_data_file);
            json data;
            file >> data;
            for (const auto& entry : data) {
                EmbeddingStoreItem item;
                item.hash = entry.value("hash", "");
                item.embedding = entry.value("embedding", std::vector<float>{});
                item.content = entry.value("content", "");
                item.timestamp = entry.value("timestamp", 0LL);
                item.frequency = entry.value("frequency", 1);
                if (!item.hash.empty()) {
                    _store[item.hash] = item;
                }
            }
            Logger::get().info("[嵌入库]", "检测到旧 JSON，开始迁移: " + _data_file);
        } catch (const std::exception& e) {
            Logger::get().warn("[嵌入库]", "旧 JSON 迁移失败: " + std::string(e.what()));
        }
    }

    char* err = nullptr;
    sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &err);
    if (err != nullptr) {
        Logger::get().warn("[嵌入库]", std::string("开启事务失败: ") + err);
        sqlite3_free(err);
        sqlite3_close(db);
        return;
    }

    const char* upsert_sql =
        "INSERT INTO embeddings(hash, content, embedding, timestamp, frequency) VALUES(?,?,?,?,?) "
        "ON CONFLICT(hash) DO UPDATE SET content=excluded.content, embedding=excluded.embedding, timestamp=excluded.timestamp, frequency=excluded.frequency;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, upsert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Logger::get().warn("[嵌入库]", "准备 upsert 语句失败: " + std::string(sqlite3_errmsg(db)));
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        return;
    }

    for (const auto& [hash, item] : _store) {
        auto blob = encodeFloatVector(item.embedding);
        sqlite3_bind_text(stmt, 1, item.hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, item.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(item.timestamp));
        sqlite3_bind_int(stmt, 5, item.frequency);

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            Logger::get().warn("[嵌入库]", "写入失败: " + std::string(sqlite3_errmsg(db)));
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_finalize(stmt);
            sqlite3_close(db);
            return;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

    sqlite3_stmt* meta_stmt = nullptr;
    const char* meta_sql = "INSERT INTO meta(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    if (sqlite3_prepare_v2(db, meta_sql, -1, &meta_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(meta_stmt, 1, "namespace", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(meta_stmt, 2, _namespace.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(meta_stmt);
        sqlite3_finalize(meta_stmt);
    }

    if (migrated) {
        try {
            fs::path backup = _data_file + ".bak";
            if (!fs::exists(backup)) {
                fs::rename(_data_file, backup);
            }
        } catch (const std::exception& e) {
            Logger::get().warn("[嵌入库]", "旧 JSON 备份失败: " + std::string(e.what()));
        }
    }

    sqlite3_close(db);
    Logger::get().info("[嵌入库]", "保存成功: " + _db_file + " (" + std::to_string(_store.size()) + " 条)");
}

void EmbeddingStore::load_from_file() {
    bool migrate_from_json = false;

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _store.clear();

        fs::create_directories(_dir_path);

        if (!fs::exists(_db_file) && fs::exists(_data_file)) {
            Logger::get().info("[嵌入库]", "检测到旧 JSON，准备迁移: " + _data_file);
            migrate_from_json = true;
        }
    }

    if (migrate_from_json) {
        save_to_file();
        return;
    }

    if (!fs::exists(_db_file)) {
        Logger::get().info("[嵌入库]", "数据库不存在，将从空库开始");
        return;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(_db_file.c_str(), &db) != SQLITE_OK) {
        Logger::get().warn("[嵌入库]", "加载 SQLite 失败: " + std::string(sqlite3_errmsg(db)));
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_busy_timeout(db, 3000);

    const char* sql = "SELECT hash, content, embedding, timestamp, frequency FROM embeddings;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Logger::get().warn("[嵌入库]", "准备查询失败: " + std::string(sqlite3_errmsg(db)));
        sqlite3_close(db);
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EmbeddingStoreItem item;
        item.hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        item.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        item.embedding = decodeFloatVector(sqlite3_column_blob(stmt, 2), sqlite3_column_bytes(stmt, 2));
        item.timestamp = static_cast<int64_t>(sqlite3_column_int64(stmt, 3));
        item.frequency = sqlite3_column_int(stmt, 4);
        _store[item.hash] = std::move(item);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    Logger::get().info("[嵌入库]", "加载成功: " + std::to_string(_store.size()) + " 条记录");
}

size_t EmbeddingStore::size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _store.size();
}

void EmbeddingStore::delete_item(const std::string& hash) {
    bool should_save = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        should_save = (_store.erase(hash) > 0);
    }
    if (should_save) {
        save_to_file();
    }
}

void EmbeddingStore::clear() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _store.clear();
    }
    Logger::get().info("[嵌入库]", "已清空: " + _namespace);
    save_to_file();
}

void EmbeddingStore::_evict_oldest() {
    size_t to_remove = _store.size() - _max_size;
    if (to_remove == 0) return;

    std::vector<std::pair<int64_t, std::string>> sorted_by_time;
    sorted_by_time.reserve(_store.size());
    for (const auto& [hash, item] : _store) {
        sorted_by_time.push_back({item.timestamp, hash});
    }
    std::sort(sorted_by_time.begin(), sorted_by_time.end());

    for (size_t i = 0; i < to_remove && i < sorted_by_time.size(); i++) {
        _store.erase(sorted_by_time[i].second);
    }

    Logger::get().info("[嵌入库]", _namespace + " 容量淘汰: 移除 " +
                       std::to_string(to_remove) + " 条最旧数据，当前 " +
                       std::to_string(_store.size()) + " 条");
}

float EmbeddingStore::cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;

    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    norm_a = std::sqrt(norm_a);
    norm_b = std::sqrt(norm_b);

    if (norm_a < 1e-6f || norm_b < 1e-6f) return 0.0f;
    return dot / (norm_a * norm_b);
}

void EmbeddingStore::save_embedding_test_vectors() {
    Logger::get().info("[嵌入库]", "开始保存测试字符串的嵌入向量...");

    auto& client = EmbeddingServiceClient::get();
    std::vector<std::string> test_strs(EMBEDDING_TEST_STRINGS.begin(), EMBEDDING_TEST_STRINGS.end());
    auto results = client.embed_texts_sync(test_strs, 5);

    json test_vectors_json;
    int failed = 0;

    for (size_t i = 0; i < test_strs.size() && i < results.size(); i++) {
        if (results[i].success && !results[i].embedding.empty()) {
            test_vectors_json[std::to_string(i)] = results[i].embedding;
        } else {
            Logger::get().warn("[嵌入库]", "获取测试字符串嵌入失败: " + test_strs[i].substr(0, 50));
            failed++;
        }
    }

    if (!fs::exists(_dir_path)) {
        fs::create_directories(_dir_path);
    }

    std::ofstream file(_test_vectors_file);
    file << test_vectors_json.dump(4);
    file.close();

    Logger::get().info("[嵌入库]", "测试字符串嵌入向量保存完成 (失败: " + std::to_string(failed) + ")");
}

std::unordered_map<std::string, std::vector<float>> EmbeddingStore::load_embedding_test_vectors() {
    std::unordered_map<std::string, std::vector<float>> result;

    if (!fs::exists(_test_vectors_file)) {
        return result;
    }

    try {
        std::ifstream file(_test_vectors_file);
        json test_vectors_json;
        file >> test_vectors_json;
        file.close();

        for (auto& [key, value] : test_vectors_json.items()) {
            if (value.is_array()) {
                result[key] = value.get<std::vector<float>>();
            }
        }
    } catch (const std::exception& e) {
        Logger::get().warn("[嵌入库]", "加载测试向量失败: " + std::string(e.what()));
    }

    return result;
}

static std::string get_current_time_string() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

ModelConsistencyCheckResult EmbeddingStore::check_embedding_model_consistency() {
    ModelConsistencyCheckResult check_result;
    check_result.model_name = "";

    auto local_vectors = load_embedding_test_vectors();

    if (local_vectors.empty()) {
        Logger::get().warn("[嵌入库]", "未检测到本地嵌入模型测试文件，将保存当前模型的测试嵌入");
        save_embedding_test_vectors();
        check_result.is_consistent = true;
        check_result.check_time = "first_initialization";
        return check_result;
    }

    for (size_t i = 0; i < EMBEDDING_TEST_STRINGS.size(); i++) {
        if (local_vectors.find(std::to_string(i)) == local_vectors.end()) {
            Logger::get().warn("[嵌入库]", "本地嵌入模型测试文件缺失部分测试字符串，将重新保存");
            save_embedding_test_vectors();
            check_result.is_consistent = true;
            check_result.check_time = "incomplete_data_reset";
            return check_result;
        }
    }

    Logger::get().info("[嵌入库]", "开始检验嵌入模型一致性...");

    auto& client = EmbeddingServiceClient::get();
    std::vector<std::string> test_strs(EMBEDDING_TEST_STRINGS.begin(), EMBEDDING_TEST_STRINGS.end());
    auto results = client.embed_texts_sync(test_strs, 5);

    check_result.is_consistent = true;
    check_result.check_time = get_current_time_string();

    for (size_t i = 0; i < test_strs.size() && i < results.size(); i++) {
        std::string idx_str = std::to_string(i);

        if (!results[i].success || results[i].embedding.empty()) {
            Logger::get().error("[嵌入库]", "获取测试字符串嵌入失败: " + test_strs[i].substr(0, 50));
            check_result.is_consistent = false;
            check_result.failed_tests.push_back(test_strs[i].substr(0, 50));
            continue;
        }

        auto local_emb = local_vectors[idx_str];
        auto new_emb = results[i].embedding;

        float sim = cosine_similarity(local_emb, new_emb);

        if (sim < EMBEDDING_SIM_THRESHOLD) {
            Logger::get().error("[嵌入库]", "嵌入模型一致性校验失败");
            Logger::get().error("[嵌入库]", "字符串: " + test_strs[i].substr(0, 50));
            Logger::get().error("[嵌入库]", "相似度: " + std::to_string(sim));

            check_result.is_consistent = false;
            check_result.failed_tests.push_back(test_strs[i].substr(0, 50));
        }
    }

    if (check_result.is_consistent) {
        Logger::get().info("[嵌入库]", "嵌入模型一致性校验通过");
    } else {
        check_result.error_message = "嵌入模型已变更，共 " + std::to_string(check_result.failed_tests.size()) + " 个测试失败";
        Logger::get().error("[嵌入库]", check_result.error_message);

        Logger::get().warn("[嵌入库]", "模型已变更，清空旧数据并重建测试向量");
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _store.clear();
        }
        save_to_file();
        save_embedding_test_vectors();
    }

    return check_result;
}

EmbeddingManager& EmbeddingManager::get() {
    static EmbeddingManager instance;
    return instance;
}

void EmbeddingManager::initialize(const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(_mutex);
    _data_dir = data_dir;

    fs::create_directories(data_dir);

    _stores["knowledge"] = std::make_unique<EmbeddingStore>("knowledge", data_dir);
    _stores["jargon"] = std::make_unique<EmbeddingStore>("jargon", data_dir);

    for (auto& [name, store] : _stores) {
        store->load_from_file();
    }

    Logger::get().info("[嵌入管理]", "初始化完成: " + std::to_string(_stores.size()) + " 个库");
}

EmbeddingStore& EmbeddingManager::get_store(const std::string& type) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_stores.find(type) == _stores.end()) {
        _stores[type] = std::make_unique<EmbeddingStore>(type, _data_dir);
        _stores[type]->load_from_file();
    }

    return *_stores[type];
}

void EmbeddingManager::save_all() {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [name, store] : _stores) {
        store->save_to_file();
    }
    Logger::get().info("[嵌入管理]", "全部保存完成");
}

bool EmbeddingManager::check_all_embedding_model_consistency() {
    std::lock_guard<std::mutex> lock(_mutex);

    Logger::get().info("[嵌入管理]", "开始检查所有库的模型一致性");

    bool all_consistent = true;
    for (auto& [type, store] : _stores) {
        auto result = store->check_embedding_model_consistency();
        if (!result.is_consistent) {
            Logger::get().error("[嵌入管理]", "库 " + type + " 模型一致性校验失败: " + result.error_message);
            all_consistent = false;
        } else {
            Logger::get().info("[嵌入管理]", "库 " + type + " 模型一致性校验通过");
        }
    }

    if (all_consistent) {
        Logger::get().info("[嵌入管理]", "所有库的模型一致性校验通过");
    } else {
        Logger::get().error("[嵌入管理]", "部分库的模型一致性校验失败，请检查embedding模型配置");
    }

    return all_consistent;
}

std::vector<std::pair<std::string, float>> EmbeddingManager::search_all(
    const std::vector<float>& query, int k, const std::string& type) {

    std::vector<std::pair<std::string, float>> all_results;

    std::lock_guard<std::mutex> lock(_mutex);

    if (!type.empty()) {
        if (_stores.find(type) != _stores.end()) {
            return _stores[type]->search_top_k(query, k);
        }
        return all_results;
    }

    for (auto& [name, store] : _stores) {
        auto results = store->search_top_k(query, k);
        all_results.insert(all_results.end(), results.begin(), results.end());
    }

    std::sort(all_results.begin(), all_results.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });

    if ((int)all_results.size() > k) {
        all_results.resize(k);
    }

    return all_results;
}
