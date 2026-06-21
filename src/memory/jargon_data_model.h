#pragma once
#include "src/core/base.h"
#include <string>
#include <ctime>

using json = nlohmann::json;

// 行话创建来源
enum class JargonCreatedBy {
    AI,     // 自动学习
    MANUAL  // 手动创建
};

// 行话数据模型（对标 MaiBot 的 MaiJargon）
struct JargonData {
    int id;                           // 自增主键
    std::string content;              // 行话内容
    std::string meaning;              // 行话含义
    std::string raw_content;          // 原始上下文（JSON 数组字符串）
    int count;                        // 使用频率
    bool is_jargon;                   // 是否为真行话（null/true/false）
    bool is_complete;                 // 是否完成推断（count >= 100）
    bool is_global;                   // 是否为全局行话
    int last_inference_count;         // 上次推断时的 count
    JargonCreatedBy created_by;       // 创建来源
    time_t created_timestamp;
    time_t updated_timestamp;
    time_t last_seen_timestamp;       // 上次看到此词汇的时间（用于时间衰减）

    // 默认构造
    JargonData()
        : id(0), count(0), is_jargon(true), is_complete(false),
          is_global(false), last_inference_count(0),
          created_by(JargonCreatedBy::AI),
          created_timestamp(time(nullptr)), updated_timestamp(time(nullptr)),
          last_seen_timestamp(time(nullptr)) {}

    // JSON 序列化
    json to_json() const {
        json j;
        j["id"] = id;
        j["content"] = content;
        j["meaning"] = meaning;
        j["raw_content"] = raw_content;
        j["count"] = count;
        j["is_jargon"] = is_jargon;
        j["is_complete"] = is_complete;
        j["is_global"] = is_global;
        j["last_inference_count"] = last_inference_count;
        j["created_by"] = (created_by == JargonCreatedBy::AI) ? "AI" : "MANUAL";
        j["created_timestamp"] = created_timestamp;
        j["updated_timestamp"] = updated_timestamp;
        j["last_seen_timestamp"] = last_seen_timestamp;
        return j;
    }

    // JSON 反序列化
    static JargonData from_json(const json& j) {
        JargonData data;
        data.id = j.value("id", 0);
        data.content = j.value("content", "");
        data.meaning = j.value("meaning", "");
        data.raw_content = j.value("raw_content", "");
        data.count = j.value("count", 0);
        data.is_jargon = j.value("is_jargon", true);
        data.is_complete = j.value("is_complete", false);
        data.is_global = j.value("is_global", false);
        data.last_inference_count = j.value("last_inference_count", 0);
        std::string created_by_str = j.value("created_by", "AI");
        data.created_by = (created_by_str == "MANUAL") ? JargonCreatedBy::MANUAL : JargonCreatedBy::AI;
        data.created_timestamp = j.value("created_timestamp", (long)time(nullptr));
        data.updated_timestamp = j.value("updated_timestamp", (long)time(nullptr));
        data.last_seen_timestamp = j.value("last_seen_timestamp", (long)time(nullptr));
        return data;
    }
};

// LLM 推理结果
struct InferenceResult {
    bool is_jargon;           // 是否为行话
    std::string meaning;      // 推断的含义
    double confidence;        // 置信度 0-1
    bool no_info;            // 信息不足无法推断
    std::string raw_response; // 原始 LLM 响应

    InferenceResult()
        : is_jargon(false), confidence(0.0), no_info(false) {}

    json to_json() const {
        json j;
        j["is_jargon"] = is_jargon;
        j["meaning"] = meaning;
        j["confidence"] = confidence;
        j["no_info"] = no_info;
        return j;
    }
};
