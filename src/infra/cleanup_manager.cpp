#include "src/infra/cleanup_manager.h"
#include "src/knowledge/embedding_store.h"
#include "src/memory/jargon_miner.h"
#include "src/infra/logger.h"
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

CleanupManager& CleanupManager::get() {
    static CleanupManager instance;
    return instance;
}

void CleanupManager::start_periodic_cleanup(int interval_seconds) {
    if (_running) return;
    _running = true;

    std::thread([this, interval_seconds]() {
        while (_running) {
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
            cleanup_expired_data();
        }
    }).detach();

    Logger::get().info("[清理管理]", "启动定期清理线程，间隔: " + std::to_string(interval_seconds) + "s");
}

void CleanupManager::set_config(const CleanupConfig& config) {
    std::lock_guard<std::mutex> lock(_mutex);
    _config = config;
    Logger::get().info("[清理管理]", "配置已更新: TTL=" + std::to_string(config.ttl_days) +
                       "天, 最小频率=" + std::to_string(config.min_frequency));
}

CleanupManager::CleanupConfig CleanupManager::get_config() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _config;
}

bool CleanupManager::_is_expired(int64_t timestamp, int ttl_days) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
    int64_t ttl_seconds = (int64_t)ttl_days * 86400;
    return (now - timestamp) > ttl_seconds;
}

int CleanupManager::_get_file_size(const std::string& path) {
    try {
        return fs::file_size(path);
    } catch (...) {
        return 0;
    }
}

void CleanupManager::_cleanup_embedding_stores() {
    Logger::get().info("[清理管理]", "开始清理 embedding 库");

    auto& manager = EmbeddingManager::get();
    auto config = get_config();

    // 遍历所有群的知识库和历史库
    std::vector<std::string> store_types = {"knowledge", "history"};

    for (const auto& store_type : store_types) {
        // 这里需要遍历所有 store_type_group_* 的库
        // 由于 EmbeddingManager 没有提供遍历接口，这里提供扩展接口
        Logger::get().debug("[清理管理] 扫描 ", store_type + " 类型的库");
    }
}

void CleanupManager::_cleanup_jargon_data() {
    Logger::get().info("[清理管理]", "开始清理行话库");

    auto& miner = JargonMiner::get();
    auto config = get_config();

    // 获取所有群的行话数据
    auto jargons_data = miner.getAllJargons();

    for (auto& [group_key, group_jargons] : jargons_data.items()) {
        if (!group_jargons.is_array()) continue;

        std::vector<size_t> indices_to_remove;

        for (size_t i = 0; i < group_jargons.size(); i++) {
            auto& entry = group_jargons[i];

            // 检查是否过期
            if (config.cleanup_expired && entry.contains("timestamp")) {
                int64_t ts = entry["timestamp"].get<int64_t>();
                if (_is_expired(ts, config.ttl_days)) {
                    indices_to_remove.push_back(i);
                    continue;
                }
            }

            // 检查是否低频
            if (config.cleanup_low_frequency && entry.contains("frequency")) {
                int freq = entry["frequency"].get<int>();
                if (freq < config.min_frequency && entry.contains("status")) {
                    std::string status = entry["status"].get<std::string>();
                    // 只清理 pending 或 rejected 状态的低频词
                    if (status != "confirmed") {
                        indices_to_remove.push_back(i);
                        continue;
                    }
                }
            }
        }

        // 反向删除（避免索引变化）
        for (int i = (int)indices_to_remove.size() - 1; i >= 0; i--) {
            group_jargons.erase(group_jargons.begin() + indices_to_remove[i]);
        }

        if (!indices_to_remove.empty()) {
            Logger::get().info("[清理管理]", "群 " + group_key + " 清理了 " +
                             std::to_string(indices_to_remove.size()) + " 个词汇");
        }
    }

    // 保存清理后的数据已由 JargonMiner 自动处理
}

void CleanupManager::cleanup_expired_data() {
    std::lock_guard<std::mutex> lock(_mutex);

    Logger::get().info("[清理管理]", "开始执行清理任务");

    auto config = get_config();
    _last_stats.timestamp = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000000
    );

    // 清理 embedding 库
    if (config.cleanup_expired) {
        _cleanup_embedding_stores();
    }

    // 清理行话库
    _cleanup_jargon_data();

    Logger::get().info("[清理管理]", "清理任务完成: 移除 " +
                       std::to_string(_last_stats.items_removed) + " 条数据");
}

void CleanupManager::cleanup_group(int64_t group_id) {
    std::lock_guard<std::mutex> lock(_mutex);

    Logger::get().info("[清理管理] 手动清理群 ", std::to_string(group_id));

    auto& manager = EmbeddingManager::get();
    manager.get_store("knowledge_group_" + std::to_string(group_id)).clear();
    manager.get_store("history_group_" + std::to_string(group_id)).clear();

    Logger::get().info("[清理管理] 群 ", std::to_string(group_id) + " 已清空");
}

CleanupManager::CleanupStats CleanupManager::get_last_stats() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _last_stats;
}

