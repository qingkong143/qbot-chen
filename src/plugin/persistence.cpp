#include "src/plugin/persistence.h"
#include "src/infra/logger.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ──────────────────────────────────────────────────────
// JSONFilePersistence 实现
// ──────────────────────────────────────────────────────

JSONFilePersistence::JSONFilePersistence(const std::string& base_dir)
    : _base_dir(base_dir) {
    ensure_dir(base_dir);
    Logger::get().info("[持久化]", "JSON文件持久化初始化，基目录: " + base_dir);
}

JSONFilePersistence::~JSONFilePersistence() {
    Logger::get().info("[持久化]", "JSON文件持久化关闭");
}

std::string JSONFilePersistence::get_file_path(const std::string& key) {
    // key 格式: "src/plugin/subkey" -> "./data/plugin/subkey.json"
    size_t pos = key.rfind('/');
    if (pos != std::string::npos) {
        std::string dir = _base_dir + "/" + key.substr(0, pos);
        std::string file = key.substr(pos + 1);
        return dir + "/" + file + ".json";
    }
    return _base_dir + "/" + key + ".json";
}

bool JSONFilePersistence::ensure_dir(const std::string& dir) {
    try {
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
        }
        return true;
    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "创建目录失败 " + dir + ": " + std::string(e.what()));
        return false;
    }
}

bool JSONFilePersistence::save(const std::string& key, const json& data) {
    try {
        std::string path = get_file_path(key);

        // 确保父目录存在
        fs::path file_path(path);
        ensure_dir(file_path.parent_path().string());

        std::ofstream file(path);
        if (!file.is_open()) {
            Logger::get().error("[持久化]", "无法打开文件写入: " + path);
            return false;
        }

        file << data.dump(2);  // 缩进2个空格
        file.close();

        Logger::get().debug("[持久化]", "已保存: " + key);
        return true;

    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "保存失败 " + key + ": " + std::string(e.what()));
        return false;
    }
}

bool JSONFilePersistence::load(const std::string& key, json& data) {
    try {
        std::string path = get_file_path(key);

        if (!fs::exists(path)) {
            Logger::get().warn("[持久化]", "文件不存在: " + path);
            return false;
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            Logger::get().error("[持久化]", "无法打开文件读取: " + path);
            return false;
        }

        file >> data;
        file.close();

        Logger::get().debug("[持久化]", "已加载: " + key);
        return true;

    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "加载失败 " + key + ": " + std::string(e.what()));
        return false;
    }
}

bool JSONFilePersistence::exists(const std::string& key) {
    std::string path = get_file_path(key);
    return fs::exists(path);
}

bool JSONFilePersistence::remove(const std::string& key) {
    try {
        std::string path = get_file_path(key);

        if (fs::exists(path)) {
            fs::remove(path);
            Logger::get().debug("[持久化]", "已删除: " + key);
            return true;
        }

        return false;

    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "删除失败 " + key + ": " + std::string(e.what()));
        return false;
    }
}

bool JSONFilePersistence::save_list(const std::string& key, const std::vector<json>& items) {
    try {
        json array = json::array();
        for (const auto& item : items) {
            array.push_back(item);
        }
        return save(key, array);

    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "保存列表失败 " + key + ": " + std::string(e.what()));
        return false;
    }
}

bool JSONFilePersistence::load_list(const std::string& key, std::vector<json>& items) {
    try {
        json data;
        if (!load(key, data)) {
            return false;
        }

        items.clear();
        if (data.is_array()) {
            for (const auto& item : data) {
                items.push_back(item);
            }
        }

        return true;

    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "加载列表失败 " + key + ": " + std::string(e.what()));
        return false;
    }
}

std::vector<std::string> JSONFilePersistence::list_keys(const std::string& prefix) {
    std::vector<std::string> keys;

    try {
        std::string search_dir = _base_dir;
        if (!prefix.empty()) {
            size_t pos = prefix.rfind('/');
            if (pos != std::string::npos) {
                search_dir = _base_dir + "/" + prefix.substr(0, pos);
            }
        }

        if (!fs::exists(search_dir)) {
            return keys;
        }

        for (const auto& entry : fs::recursive_directory_iterator(search_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                std::string rel_path = fs::relative(entry.path(), _base_dir).string();
                // 移除 .json 后缀和路径分隔符
                rel_path = rel_path.substr(0, rel_path.length() - 5);

                // 转换为 "/" 分隔符
                #ifdef _WIN32
                std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
                #endif

                if (prefix.empty() || rel_path.find(prefix) == 0) {
                    keys.push_back(rel_path);
                }
            }
        }

    } catch (const std::exception& e) {
        Logger::get().error("[持久化]", "列出键失败: " + std::string(e.what()));
    }

    return keys;
}

// ──────────────────────────────────────────────────────
// PersistenceManager 实现
// ──────────────────────────────────────────────────────

PersistenceManager& PersistenceManager::get() {
    static PersistenceManager instance;
    return instance;
}

PersistenceManager::PersistenceManager()
    : _provider(nullptr) {
}

void PersistenceManager::initialize(const std::string& base_dir) {
    _provider = std::make_shared<JSONFilePersistence>(base_dir);
    Logger::get().info("[持久化管理]", "初始化完成，使用 JSON 文件存储");
}

void PersistenceManager::set_provider(std::shared_ptr<PersistenceProvider> provider) {
    if (!provider) {
        Logger::get().error("[持久化管理]", "尝试设置空提供者");
        return;
    }
    _provider = provider;
    Logger::get().info("[持久化管理]", "存储提供者已更改");
}

bool PersistenceManager::save(const std::string& key, const json& data) {
    if (!_provider) {
        Logger::get().error("[持久化管理]", "存储提供者未初始化");
        return false;
    }
    return _provider->save(key, data);
}

bool PersistenceManager::load(const std::string& key, json& data) {
    if (!_provider) {
        Logger::get().error("[持久化管理]", "存储提供者未初始化");
        return false;
    }
    return _provider->load(key, data);
}

bool PersistenceManager::exists(const std::string& key) {
    if (!_provider) {
        return false;
    }
    return _provider->exists(key);
}

bool PersistenceManager::remove(const std::string& key) {
    if (!_provider) {
        Logger::get().error("[持久化管理]", "存储提供者未初始化");
        return false;
    }
    return _provider->remove(key);
}

bool PersistenceManager::save_list(const std::string& key, const std::vector<json>& items) {
    if (!_provider) {
        Logger::get().error("[持久化管理]", "存储提供者未初始化");
        return false;
    }
    return _provider->save_list(key, items);
}

bool PersistenceManager::load_list(const std::string& key, std::vector<json>& items) {
    if (!_provider) {
        Logger::get().error("[持久化管理]", "存储提供者未初始化");
        return false;
    }
    return _provider->load_list(key, items);
}

std::vector<std::string> PersistenceManager::list_keys(const std::string& prefix) {
    if (!_provider) {
        return {};
    }
    return _provider->list_keys(prefix);
}


