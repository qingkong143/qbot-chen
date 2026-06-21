#include "src/knowledge/embedding_service.h"
#include "src/core/config.h"
#include "src/infra/logger.h"
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <thread>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <algorithm>

EmbeddingServiceClient& EmbeddingServiceClient::get() {
    static EmbeddingServiceClient instance;
    return instance;
}

void EmbeddingServiceClient::initialize(const std::string& api_url, const std::string& api_key,
                                        const std::string& model) {
    _api_url = api_url;
    _api_key = api_key;
    _model = model;
    Logger::get().info("[向量服务]", "初始化完成: model=" + model + ", url=" + api_url);
}

std::string EmbeddingServiceClient::_hash_text(const std::string& text) const {
    // 简单的哈希（实际可用 SHA256）
    std::hash<std::string> hasher;
    return std::to_string(hasher(text));
}

// CURL 回调函数
static size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

EmbeddingResult EmbeddingServiceClient::_call_api(const std::string& text) {
    EmbeddingResult result;
    result.model_name = _model;

    std::string api_url = _api_url;
    std::string api_key = _api_key;
    std::string model = _model;

    // 第一层回退：embedding 配置
    const auto& emb_cfg = Config::get().embedding();
    if (api_url.empty()) api_url = emb_cfg.api_url;
    if (api_key.empty()) api_key = emb_cfg.api_key;
    if (model.empty() || model == "deepseek-chat") model = emb_cfg.model;

    // 第二层回退：main_model 配置（config.cpp 注释已说明 embedding 配置已移除，共用主模型）
    if (api_url.empty() || api_key.empty()) {
        const auto& main_cfg = Config::get().main_model();
        if (api_url.empty()) api_url = main_cfg.url;
        if (api_key.empty()) api_key = main_cfg.api_key;
        if (model.empty() || model == "deepseek-chat") model = main_cfg.model;
    }

    if (api_url.empty() || api_key.empty()) {
        result.error_msg = "Embedding API 配置缺失（embedding 和 main_model 均未设置 api_url/api_key）";
        Logger::get().warn("[向量服务]", result.error_msg);
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error_msg = "CURL 初始化失败";
        return result;
    }

    try {
        json request;
        request["input"] = text;
        request["model"] = model;
        std::string request_body = request.dump();

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key;
        headers = curl_slist_append(headers, auth_header.c_str());

        std::string response_body;
        long http_code = 0;

        // 重试：网络错误时最多重试 1 次
        for (int attempt = 0; attempt < 2; attempt++) {
            response_body.clear();
            curl_easy_reset(curl);

            curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

            CURLcode res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            if (res == CURLE_OK && http_code >= 200 && http_code < 300 && !response_body.empty()) {
                break; // 成功
            }

            if (attempt == 0) {
                std::string reason;
                if (res != CURLE_OK) {
                    reason = "CURL 错误: " + std::string(curl_easy_strerror(res));
                } else if (http_code == 401) {
                    reason = "API Key 认证失败 (401)";
                } else if (http_code == 403) {
                    reason = "API 权限不足 (403)";
                } else if (http_code == 429) {
                    reason = "API 请求过于频繁 (429)";
                } else if (http_code >= 500) {
                    reason = "API 服务端错误 (" + std::to_string(http_code) + ")";
                } else if (response_body.empty()) {
                    reason = "API 响应为空";
                }
                Logger::get().warn("[向量服务]", "请求失败 (attempt " + std::to_string(attempt + 1) + "/2): " + reason);
            }
        }

        curl_slist_free_all(headers);

        if (http_code < 200 || http_code >= 300) {
            result.error_msg = "HTTP 错误: " + std::to_string(http_code) + ", url=" + api_url;
            Logger::get().warn("[向量服务]", result.error_msg);
            curl_easy_cleanup(curl);
            return result;
        }

        if (response_body.empty()) {
            result.error_msg = "API 响应为空（重试后仍失败）";
            Logger::get().warn("[向量服务]", result.error_msg);
            curl_easy_cleanup(curl);
            return result;
        }

        try {
            json response = json::parse(response_body);

            if (response.contains("data") && response["data"].is_array() && response["data"].size() > 0) {
                auto embedding = response["data"][0]["embedding"];
                if (embedding.is_array()) {
                    result.embedding = embedding.get<std::vector<float>>();
                    result.success = true;
                    Logger::get().debug("[向量服务]", "成功获取向量: " + std::to_string(result.embedding.size()) + " 维");
                }
            } else {
                std::string err_detail = response.value("error", json::object()).value("message", response_body.substr(0, 200));
                result.error_msg = "API 响应格式错误: " + err_detail;
                Logger::get().warn("[向量服务]", result.error_msg);
            }
        } catch (const std::exception& e) {
            result.error_msg = "JSON 解析失败: " + std::string(e.what());
            Logger::get().warn("[向量服务]", result.error_msg);
        }

    } catch (const std::exception& e) {
        result.error_msg = "异常: " + std::string(e.what());
    }

    curl_easy_cleanup(curl);
    return result;
}

EmbeddingResult EmbeddingServiceClient::embed_text_sync(const std::string& text) {
    // 检查缓存并清理过期项
    std::string text_hash = _hash_text(text);
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);

        // 清理过期的缓存
        _evict_expired_entries();

        auto it = _cache.find(text_hash);
        if (it != _cache.end()) {
            // ✅ 缓存命中：更新访问时间和计数
            it->second.access_time = _get_current_timestamp();
            it->second.access_count++;
            _total_cache_hits++;
            Logger::get().debug("[向量服务]", "缓存命中 (命中率: " +
                std::to_string(_total_cache_hits * 100 / (_total_cache_hits + _total_cache_misses + 1)) + "%)");
            return it->second.result;
        }
    }

    _total_cache_misses++;

    // 调用 API
    EmbeddingResult result = _call_api(text);

    // 存入缓存
    if (result.success) {
        std::lock_guard<std::mutex> lock(_cache_mutex);

        // 检查是否需要清理缓存
        if (_should_evict()) {
            Logger::get().debug("[向量服务]", "缓存满，执行LRU清理");
            _evict_lru_entries();
        }

        CacheEntry entry;
        entry.result = result;
        entry.create_time = _get_current_timestamp();
        entry.access_time = entry.create_time;
        entry.access_count = 1;

        _cache[text_hash] = entry;
    }

    return result;
}

std::vector<EmbeddingResult> EmbeddingServiceClient::embed_texts_sync(
    const std::vector<std::string>& texts, int max_concurrent) {

    std::vector<EmbeddingResult> results;

    if (texts.empty()) {
        return results;
    }

    // 单线程模式
    if (max_concurrent <= 1) {
        for (const auto& text : texts) {
            results.push_back(embed_text_sync(text));
        }
        return results;
    }

    // 多线程模式（简化版：不依赖外部 ThreadPool 库）
    std::queue<std::pair<size_t, std::string>> task_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    std::atomic<int> active_threads(0);
    std::vector<EmbeddingResult> thread_results(texts.size());
    bool shutdown = false;

    // 初始化任务队列
    for (size_t i = 0; i < texts.size(); i++) {
        task_queue.push({i, texts[i]});
    }

    // 工作线程函数
    auto worker = [this, &task_queue, &queue_mutex, &cv, &active_threads, &thread_results, &shutdown]() {
        while (true) {
            std::pair<size_t, std::string> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cv.wait(lock, [&] { return !task_queue.empty() || shutdown; });
                if (task_queue.empty()) break;
                task = task_queue.front();
                task_queue.pop();
            }

            thread_results[task.first] = embed_text_sync(task.second);
        }
        active_threads--;
    };

    // 启动工作线程
    std::vector<std::thread> threads;
    int num_threads = std::min((int)texts.size(), max_concurrent);
    for (int i = 0; i < num_threads; i++) {
        active_threads++;
        threads.emplace_back(worker);
    }

    // 唤醒所有线程
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        shutdown = true;
    }
    cv.notify_all();

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    results = thread_results;
    return results;
}

void EmbeddingServiceClient::clear_cache() {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    _cache.clear();
    _total_cache_hits = 0;
    _total_cache_misses = 0;
    Logger::get().info("[向量服务]", "缓存已清空");
}

// ========== 新增：缓存管理方法实现 ==========

void EmbeddingServiceClient::set_cache_max_entries(size_t max_entries) {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    _cache_max_entries = std::max(size_t(1), max_entries);
    Logger::get().info("[向量服务]", "缓存最大条目数设置为: " + std::to_string(_cache_max_entries));
}

void EmbeddingServiceClient::set_cache_max_size_mb(size_t max_size_mb) {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    _cache_max_size_bytes = std::max(size_t(1), max_size_mb) * 1024 * 1024;
    Logger::get().info("[向量服务]", "缓存最大大小设置为: " + std::to_string(max_size_mb) + " MB");
}

size_t EmbeddingServiceClient::get_cache_entries_count() const {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    return _cache.size();
}

size_t EmbeddingServiceClient::get_cache_size() const {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    return _estimate_cache_size();
}

void EmbeddingServiceClient::print_cache_stats() {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    size_t total_requests = _total_cache_hits + _total_cache_misses;
    float hit_rate = total_requests > 0 ? (100.0f * _total_cache_hits / total_requests) : 0.0f;
    size_t cache_size = _estimate_cache_size();
    size_t cache_size_mb = cache_size / (1024 * 1024);

    Logger::get().info("[向量服务缓存统计]", "条目数: " + std::to_string(_cache.size()) +
                       ", 大小: " + std::to_string(cache_size_mb) + " MB" +
                       ", 命中率: " + std::to_string((int)hit_rate) + "% " +
                       "(" + std::to_string(_total_cache_hits) + "/" + std::to_string(total_requests) + ")");
}

int64_t EmbeddingServiceClient::_get_current_timestamp() {
    return std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
}

size_t EmbeddingServiceClient::_estimate_entry_size(const EmbeddingResult& result) {
    // 估算：string (model_name, error_msg) + vector (embedding) + overhead
    size_t size = result.model_name.size() + result.error_msg.size();
    size += result.embedding.size() * sizeof(float);
    size += 100;  // 结构体开销
    return size;
}

size_t EmbeddingServiceClient::_estimate_cache_size() const {
    size_t total = 0;
    for (const auto& [key, entry] : _cache) {
        total += key.size();  // hash key
        total += _estimate_entry_size(entry.result);
        total += 50;  // CacheEntry 元数据开销
    }
    return total;
}

bool EmbeddingServiceClient::_should_evict() const {
    // 检查是否超过条目限制或大小限制
    if (_cache.size() >= _cache_max_entries) return true;
    if (_estimate_cache_size() >= _cache_max_size_bytes) return true;
    return false;
}

void EmbeddingServiceClient::_evict_expired_entries() {
    int64_t now = _get_current_timestamp();
    int evicted = 0;

    for (auto it = _cache.begin(); it != _cache.end(); ) {
        if ((now - it->second.create_time) > _cache_ttl_seconds) {
            it = _cache.erase(it);
            evicted++;
        } else {
            ++it;
        }
    }

    if (evicted > 0) {
        Logger::get().debug("[向量服务]", "清理过期缓存: " + std::to_string(evicted) + " 条");
    }
}

void EmbeddingServiceClient::_evict_lru_entries() {
    // 清理访问频率最低的30%条目
    size_t target_size = _cache.size() * 7 / 10;  // 目标为当前的70%

    std::vector<std::pair<std::string, int>> entries_by_access;
    for (const auto& [key, entry] : _cache) {
        entries_by_access.push_back({key, entry.access_count});
    }

    // 按访问计数排序
    std::sort(entries_by_access.begin(), entries_by_access.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // 删除访问计数最低的条目
    int evicted = 0;
    for (size_t i = 0; i < entries_by_access.size() && _cache.size() > target_size; i++) {
        _cache.erase(entries_by_access[i].first);
        evicted++;
    }

    Logger::get().debug("[向量服务]", "LRU清理: 删除 " + std::to_string(evicted) + " 条，当前条目: " +
                       std::to_string(_cache.size()));
}
