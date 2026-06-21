#pragma once

#include "src/core/base.h"
#include <map>
#include <memory>
#include <string>

using json = nlohmann::json;

// ──────────────────────────────────────────────────────
// 持久化接口
// ──────────────────────────────────────────────────────

class PersistenceProvider {
public:
    virtual ~PersistenceProvider() = default;

    // 基础操作
    virtual bool save(const std::string& key, const json& data) = 0;
    virtual bool load(const std::string& key, json& data) = 0;
    virtual bool exists(const std::string& key) = 0;
    virtual bool remove(const std::string& key) = 0;

    // 集合操作
    virtual bool save_list(const std::string& key, const std::vector<json>& items) = 0;
    virtual bool load_list(const std::string& key, std::vector<json>& items) = 0;

    // 查询操作
    virtual std::vector<std::string> list_keys(const std::string& prefix) = 0;
};

// ──────────────────────────────────────────────────────
// JSON 文件持久化实现
// ──────────────────────────────────────────────────────

class JSONFilePersistence : public PersistenceProvider {
public:
    JSONFilePersistence(const std::string& base_dir = "./data");
    ~JSONFilePersistence() override;

    bool save(const std::string& key, const json& data) override;
    bool load(const std::string& key, json& data) override;
    bool exists(const std::string& key) override;
    bool remove(const std::string& key) override;

    bool save_list(const std::string& key, const std::vector<json>& items) override;
    bool load_list(const std::string& key, std::vector<json>& items) override;

    std::vector<std::string> list_keys(const std::string& prefix) override;

private:
    std::string _base_dir;
    std::string get_file_path(const std::string& key);
    bool ensure_dir(const std::string& dir);
};

// ──────────────────────────────────────────────────────
// 持久化管理器（单例）
// ──────────────────────────────────────────────────────

class PersistenceManager {
public:
    static PersistenceManager& get();

    // 初始化
    void initialize(const std::string& base_dir = "./data");
    void set_provider(std::shared_ptr<PersistenceProvider> provider);

    // 代理接口
    bool save(const std::string& key, const json& data);
    bool load(const std::string& key, json& data);
    bool exists(const std::string& key);
    bool remove(const std::string& key);

    bool save_list(const std::string& key, const std::vector<json>& items);
    bool load_list(const std::string& key, std::vector<json>& items);

    std::vector<std::string> list_keys(const std::string& prefix);

private:
    PersistenceManager();
    std::shared_ptr<PersistenceProvider> _provider;
};
