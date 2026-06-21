#pragma once
#include "base.h"
#include <map>
#include <set>
#include <vector>

using json = nlohmann::json;

// 模型配置
struct ModelConfig {
    std::string provider;     // 厂商名，仅做标记
    std::string model;        // 模型名
    std::string url;          // API 端点
    std::string api_key;      // API 密钥
    double temperature = 0.3;
    int max_tokens = 65536;   // 单次输出上限
    int context_window = 1000000; // 上下文窗口大小，用于自动推导压缩阈值
};

// Embedding 配置（向量检索独立配置）
struct EmbeddingConfig {
    std::string api_url;           // 完整的 embedding API 端点（如：https://api.openai.com/v1/embeddings）
    std::string api_key;
    std::string model;             // embedding 模型名
    std::string provider = "openai"; // 提供商标记：openai / deepseek / custom
};

// OCR / 图片识别配置
struct OcrConfig {
    bool enabled = true;
    std::string provider = "siliconflow";
    std::string model = "deepseek-ai/DeepSeek-OCR";
    std::string url = "https://api.siliconflow.cn/v1/chat/completions";
    std::string api_key;
    int max_images_per_message = 2;
    int timeout_seconds = 45;
};


// 消息管线配置
struct PipelineConfig {
    double silentChance = 1.0;            // 兼容旧配置：未@消息进入评估的概率
    double unmentionedEvalChance = 1.0;   // 未@群消息进入 LLM 评估的概率
    int idleCompensationSeconds = 10800;  // 群聊空闲补偿触发间隔（秒）= 3 小时
    int idleQuietStartHour = 0;           // 空闲补偿静默时段开始小时（0点）
    int idleQuietEndHour = 7;             // 空闲补偿静默时段结束小时（7点）→ 半夜 0-7 点不触发
    int cooldownSeconds = 30;             // 两次回复的最小间隔（秒）
    std::string logLevel = "info";        // 日志级别：debug/info/warn/error
};

// 屏蔽词配置
struct BanConfig {
    std::vector<std::string> ban_words;
    std::vector<std::string> ban_regex;
    std::vector<std::string> ad_keywords;
};

// 命令配置
struct CommandsConfig {
    struct Entry {
        std::string prefix;
        std::string name;
        std::string description;
    };
    std::vector<Entry> commands;
};

// A_Memorix 风格长期记忆配置
struct AMemorixConfig {
    bool enabled = true;
    std::string data_dir = "data/a-memorix";
    bool enable_query = true;
    bool inject_profile = true;
    bool write_person_facts = true;
    bool write_chat_summary = true;
    bool feedback_correction = false;
    bool after_reply = true;
    int message_window = 30;
    double min_importance = 0.65;
    int summary_interval_minutes = 30;
    int max_facts = 8;
    int max_episodes = 3;
    int max_profile_chars = 800;
};

// NapCat QQ Bot 配置
struct NapcatConfig {
    std::string ws_url   = "ws://127.0.0.1:3001";
    std::string http_url = "http://127.0.0.1:3000";
    std::string token    = "cwgL9xF8LgAwBp0I";
    std::set<int64_t> group_whitelist;    // 空=允许所有群
    std::set<int64_t> private_whitelist;  // 空=允许所有私聊
    std::set<std::string> admin_users;    // 允许 exec_cmd 的 QQ 号
    std::vector<std::string> bot_aliases; // 未@消息中用于判断是否在叫机器人的昵称
    std::string identity_rules_suffix;
    size_t max_private_agents = 500;  // 私聊 agent 上限，超出后 LRU 淘汰
};

// MCP Server 配置
struct McpServerEntry {
    std::string name;
    std::string url;
    std::string transport = "streamable_http";
    std::string api_key;
    std::map<std::string, std::string> headers; // 任意自定义 HTTP 头
    bool enabled = true;
};

using McpServersConfig = std::vector<McpServerEntry>;

// 全局 Config 单例
class Config {
public:
    static Config& get();

    // 从 JSON 文件加载，失败则用默认值
    void load(const std::string& path = "config.json");
    void reload() { load(_lastPath); }

    const ModelConfig&  main_model()    const { return _main; }
    const ModelConfig&  summary_model() const { return _summary; }
    const EmbeddingConfig& embedding()  const { return _embedding; }
    const OcrConfig& ocr()              const { return _ocr; }
    const NapcatConfig& napcat()        const { return _napcat; }
    const std::string&  system_prompt() const { return _sys_prompt; }
    const PipelineConfig& pipeline()      const { return _pipeline; }
    const BanConfig&     ban()            const { return _ban; }
    const CommandsConfig& commands()      const { return _commands; }
    const AMemorixConfig& a_memorix()     const { return _a_memorix; }
    const McpServersConfig& mcp_servers() const { return _mcp_servers; }

private:
    Config();  // 构造时填入默认值

    ModelConfig  _main;
    ModelConfig  _summary;
    EmbeddingConfig _embedding;
    OcrConfig _ocr;
    NapcatConfig _napcat;
    std::string  _sys_prompt;
    std::string  _lastPath;
    PipelineConfig _pipeline;
    BanConfig _ban;
    CommandsConfig _commands;
    AMemorixConfig _a_memorix;
    McpServersConfig _mcp_servers;
};
