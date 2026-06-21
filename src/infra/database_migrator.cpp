#include "src/infra/database_migrator.h"
#include "src/memory/jargon_miner.h"
#include "src/knowledge/embedding_store.h"
#include "src/infra/logger.h"
#include <fstream>
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

DatabaseMigrator& DatabaseMigrator::get() {
    static DatabaseMigrator instance;
    return instance;
}

int DatabaseMigrator::get_db_version() const {
    try {
        if (fs::exists("jargons.json")) {
            std::ifstream file("jargons.json");
            json data;
            file >> data;

            if (data.contains(VERSION_KEY)) {
                return data[VERSION_KEY].get<int>();
            }
        }
    } catch (...) {
        // 文件不存在或无法读取
    }

    return 1;  // 默认版本 1（旧格式）
}

void DatabaseMigrator::set_db_version(int version) {
    try {
        json data;
        if (fs::exists("jargons.json")) {
            std::ifstream file("jargons.json");
            file >> data;
        }

        data[VERSION_KEY] = version;

        std::ofstream file("jargons.json");
        file << data.dump(2);
        Logger::get().info("[迁移] 数据库版本已更新为: ", std::to_string(version));
    } catch (const std::exception& e) {
        Logger::get().warn("[迁移] 更新版本失败: ", std::string(e.what()));
    }
}

void DatabaseMigrator::backup_original_data() {
    try {
        auto now = std::time(nullptr);
        auto tm = *std::localtime(&now);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
        std::string timestamp = oss.str();

        std::string backup_dir = "data/backups";
        fs::create_directories(backup_dir);

        // 备份 jargons.json
        if (fs::exists("jargons.json")) {
            std::string backup_file = backup_dir + "/jargons_" + timestamp + ".json.bak";
            fs::copy_file("jargons.json", backup_file);
            Logger::get().info("[迁移] 已备份: ", backup_file);
        }

        // 备份 embedding 库
        if (fs::exists("data/embedding")) {
            std::string backup_embedding = backup_dir + "/embedding_" + timestamp;
            fs::create_directories(backup_embedding);
            for (const auto& entry : fs::recursive_directory_iterator("data/embedding")) {
                if (entry.is_regular_file()) {
                    fs::copy_file(entry.path(),
                                 backup_embedding + "/" + entry.path().filename().string());
                }
            }
            Logger::get().info("[迁移]", "已备份 embedding 库");
        }
    } catch (const std::exception& e) {
        Logger::get().warn("[迁移] 备份失败: ", std::string(e.what()));
    }
}

void DatabaseMigrator::_add_default_fields(json& entry) {
    // 添加缺失的字段到旧的行话条目

    if (!entry.contains("raw_content")) {
        entry["raw_content"] = "";
    }

    if (!entry.contains("is_jargon")) {
        entry["is_jargon"] = json(nullptr);  // null = 未判定
    }

    if (!entry.contains("status")) {
        entry["status"] = "pending";
    }

    if (!entry.contains("meaning")) {
        entry["meaning"] = "";
    }

    if (!entry.contains("frequency")) {
        entry["frequency"] = 1;
    }

    if (!entry.contains("user_id")) {
        entry["user_id"] = "";
    }

    if (!entry.contains("timestamp")) {
        entry["timestamp"] = std::time(nullptr);
    }
}

void DatabaseMigrator::_migrate_v1_to_v2() {
    Logger::get().info("[迁移]", "开始从 v1 迁移到 v2");

    try {
        if (!fs::exists("jargons.json")) {
            Logger::get().info("[迁移]", "jargons.json 不存在，跳过迁移");
            return;
        }

        std::ifstream file("jargons.json");
        json data;
        file >> data;
        file.close();

        // 为每个群的行话条目添加缺失字段
        for (auto& [group_key, group_jargons] : data.items()) {
            if (group_key == VERSION_KEY) continue;  // 跳过版本字段

            if (group_jargons.is_array()) {
                for (auto& entry : group_jargons) {
                    _add_default_fields(entry);
                }
                Logger::get().debug("[迁移] 群 ", group_key + " 已升级");
            }
        }

        // 写回更新后的数据
        std::ofstream out_file("jargons.json");
        out_file << data.dump(2);
        out_file.close();

        Logger::get().info("[迁移]", "v1->v2 迁移完成");
    } catch (const std::exception& e) {
        Logger::get().warn("[迁移] v1->v2 迁移失败: ", std::string(e.what()));
    }
}

void DatabaseMigrator::migrate_jargons_schema() {
    int current_version = get_db_version();

    if (current_version < 2) {
        _migrate_v1_to_v2();
    }

    set_db_version(CURRENT_VERSION);
}

void DatabaseMigrator::migrate_embedding_stores() {
    Logger::get().info("[迁移]", "检查 embedding 库完整性");

    try {
        fs::path embedding_dir = "data/embedding";
        if (!fs::exists(embedding_dir)) {
            fs::create_directories(embedding_dir);
            Logger::get().info("[迁移]", "已创建 data/embedding 目录");
        }

        for (const auto& entry : fs::directory_iterator(embedding_dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }

            fs::path json_file = entry.path();
            fs::path db_file = json_file;
            db_file.replace_extension(".db");

            if (fs::exists(db_file)) {
                continue;
            }

            Logger::get().info("[迁移]", "发现旧 embedding JSON，等待 EmbeddingStore 自动迁移: " + json_file.string());
        }
    } catch (const std::exception& e) {
        Logger::get().warn("[迁移] embedding 库迁移失败: ", std::string(e.what()));
    }
}

void DatabaseMigrator::migrate_config() {
    Logger::get().info("[迁移]", "检查配置完整性");

    try {
        if (!fs::exists("config.json")) {
            Logger::get().warn("[迁移]", "config.json 不存在，请创建配置文件");
        }
    } catch (const std::exception& e) {
        Logger::get().warn("[迁移] 配置检查失败: ", std::string(e.what()));
    }
}

void DatabaseMigrator::initialize_missing_fields() {
    Logger::get().info("[迁移]", "初始化缺失字段");

    try {
        if (fs::exists("jargons.json")) {
            std::ifstream file("jargons.json");
            json data;
            file >> data;
            file.close();

            bool modified = false;
            for (auto& [group_key, group_jargons] : data.items()) {
                if (group_key == VERSION_KEY) continue;

                if (group_jargons.is_array()) {
                    for (auto& entry : group_jargons) {
                        size_t before = entry.size();
                        _add_default_fields(entry);
                        if (entry.size() > before) {
                            modified = true;
                        }
                    }
                }
            }

            if (modified) {
                std::ofstream out_file("jargons.json");
                out_file << data.dump(2);
                out_file.close();
                Logger::get().info("[迁移]", "缺失字段已补充");
            }
        }
    } catch (const std::exception& e) {
        Logger::get().warn("[迁移] 初始化失败: ", std::string(e.what()));
    }
}

DatabaseMigrator::ValidationReport DatabaseMigrator::validate_all_data() {
    ValidationReport report;
    report.timestamp = std::to_string(std::time(nullptr));

    Logger::get().info("[迁移]", "开始数据验证");

    try {
        // 验证 jargons.json
        if (fs::exists("jargons.json")) {
            std::ifstream file("jargons.json");
            if (!file.good()) {
                report.issues.push_back("无法打开 jargons.json 文件");
                report.is_valid = false;
            } else {
                json data;
                if (file.peek() == std::ifstream::traits_type::eof()) {
                    // 文件为空，这是正常的初始化状态
                    report.is_valid = true;
                } else {
                    file >> data;

                    for (auto& [group_key, group_jargons] : data.items()) {
                        if (group_key == VERSION_KEY) continue;

                        if (!group_jargons.is_array()) {
                            report.issues.push_back("群 " + group_key + " 的数据格式错误");
                            report.is_valid = false;
                            continue;
                        }

                        for (size_t i = 0; i < group_jargons.size(); i++) {
                            const auto& entry = group_jargons[i];

                            if (!entry.contains("term")) {
                                report.issues.push_back("群 " + group_key + " 的第 " +
                                                      std::to_string(i) + " 条缺少 term 字段");
                                report.is_valid = false;
                            }

                            if (!entry.contains("frequency") || entry["frequency"].get<int>() < 1) {
                                report.warnings.push_back("群 " + group_key + " 的 " +
                                                         (entry.contains("term") ?
                                                          entry["term"].get<std::string>() : "?") +
                                                         " 频率异常");
                            }
                        }
                    }
                }
            }
        }

        // 验证 embedding 库
        if (fs::exists("data/embedding")) {
            int file_count = 0;
            for (const auto& entry : fs::directory_iterator("data/embedding")) {
                if (entry.is_regular_file()) {
                    file_count++;
                }
            }
            if (file_count == 0) {
                report.warnings.push_back("embedding 库为空");
            }
        }

        if (report.is_valid) {
            Logger::get().info("[迁移]", "数据验证通过");
        } else {
            Logger::get().warn("[迁移]", "数据验证发现问题");
        }

    } catch (const std::exception& e) {
        report.issues.push_back("验证过程异常: " + std::string(e.what()));
        report.is_valid = false;
    }

    return report;
}

void DatabaseMigrator::repair_data() {
    Logger::get().info("[迁移]", "开始数据修复");

    // 执行所有修复步骤
    backup_original_data();
    initialize_missing_fields();
    migrate_jargons_schema();
    migrate_embedding_stores();

    auto report = validate_all_data();
    if (report.is_valid) {
        Logger::get().info("[迁移]", "数据修复完成");
    } else {
        Logger::get().warn("[迁移]", "数据修复发现问题，请检查日志");
        for (const auto& issue : report.issues) {
            Logger::get().warn("[迁移] 问题: ", issue);
        }
    }
}

void DatabaseMigrator::migrate_all() {
    Logger::get().info("[迁移]", "执行完整数据库迁移流程");

    repair_data();

    Logger::get().info("[迁移]", "迁移流程完成");
}

