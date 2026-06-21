#include "config.h"
#include <fstream>
#include <iostream>

// ── 内置默认值（仅用于可选字段，不用于必填项） ──
Config::Config() {
    // 可选数值默认值
    _main.temperature    = 0.3;
    _main.max_tokens     = 65536;
    _main.context_window = 1000000;

    _summary.temperature    = 0.3;
    _summary.max_tokens     = 65500;
    _summary.context_window = 128000;

    _ocr.enabled = false;
    _ocr.max_images_per_message = 2;
    _ocr.timeout_seconds = 45;

    _pipeline.silentChance = 1.0;
    _pipeline.unmentionedEvalChance = 1.0;
    _pipeline.idleCompensationSeconds = 10800;
    _pipeline.idleQuietStartHour = 0;
    _pipeline.idleQuietEndHour = 7;
    _pipeline.cooldownSeconds = 30;

    _a_memorix.enabled = true;
    _a_memorix.data_dir = "data/a-memorix";
    _a_memorix.enable_query = true;
    _a_memorix.inject_profile = true;
    _a_memorix.write_person_facts = true;
    _a_memorix.write_chat_summary = true;
    _a_memorix.feedback_correction = false;
    _a_memorix.after_reply = true;
    _a_memorix.message_window = 30;
    _a_memorix.min_importance = 0.65;
    _a_memorix.summary_interval_minutes = 30;
    _a_memorix.max_facts = 8;
    _a_memorix.max_episodes = 3;
    _a_memorix.max_profile_chars = 800;

    _sys_prompt = "你是一个由开发者[chenshuzhe143]缔造的智能生命体";
}

Config& Config::get() {
    static Config instance;
    return instance;
}

// ── JSON 辅助函数 ──
static std::string json_str(const json& j, const std::string& key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}
static double json_num(const json& j, const std::string& key, double def = 0) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<double>();
    return def;
}
static int json_int(const json& j, const std::string& key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<int>();
    return def;
}
static bool json_bool(const json& j, const std::string& key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

static void parse_model(const json& src, ModelConfig& dst) {
    if (!src.is_object()) return;
    std::string v;
    v = json_str(src, "provider");    if (!v.empty()) dst.provider = v;
    v = json_str(src, "model");       if (!v.empty()) dst.model = v;
    v = json_str(src, "url");         if (!v.empty()) dst.url = v;
    v = json_str(src, "api_key");     if (!v.empty()) dst.api_key = v;
    double d = json_num(src, "temperature", -1);
    if (d >= 0) dst.temperature = d;
    int n = json_int(src, "max_tokens", -1);
    if (n > 0) dst.max_tokens = n;
    n = json_int(src, "context_window", -1);
    if (n > 0) dst.context_window = n;
}

// ── 模板 config.json 内容 ──
static const char* CONFIG_TEMPLATE = R"JSON(
{
    "main_model": {
        "provider": "deepseek",
        "model": "deepseek-v4-flash",
        "url": "https://api.deepseek.com/chat/completions",
        "api_key": "sk-your-api-key",
        "temperature": 0.3,
        "max_tokens": 65536,
        "context_window": 1000000
    },
    "summary_model": {
        "provider": "your-provider",
        "model": "your-summary-model",
        "url": "https://api.example.com/v1/chat/completions",
        "api_key": "sk-your-api-key",
        "temperature": 0.3,
        "max_tokens": 65500,
        "context_window": 128000
    },
    "embedding": {
        "api_url": "https://api.example.com/v1/embeddings",
        "api_key": "sk-your-embedding-api-key",
        "model": "your-embedding-model"
    },
    "ocr": {
        "enabled": false,
        "provider": "siliconflow",
        "model": "deepseek-ai/DeepSeek-OCR",
        "url": "https://api.siliconflow.cn/v1/chat/completions",
        "api_key": "sk-your-ocr-api-key",
        "max_images_per_message": 2,
        "timeout_seconds": 45
    },
    "napcat": {
        "ws_url": "ws://127.0.0.1:3001",
        "http_url": "http://127.0.0.1:3000",
        "token": "your-napcat-token",
        "group_whitelist": [],
        "private_whitelist": [],
        "admin_users": ["your-qq-number"],
        "bot_aliases": [],
        "max_private_agents": 500
    },
    "mcp_servers": [
        {
            "name": "fetch",
            "url": "https://mcp.api-inference.modelscope.net/xxx/mcp",
            "transport": "streamable_http",
            "headers": {
                "Authorization": "Bearer your-api-key"
            },
            "enabled": true
        }
    ],
    "pipeline": {
        "cooldownSeconds": 30
    },
    "system_prompt": "你是一个由开发者[chenshuzhe143]缔造的智能生命体"
}
)JSON";

// ── 必填项验证 ──
static std::vector<std::string> validate_required(const json& cfg) {
    std::vector<std::string> missing;

    auto require_str = [&](const json& obj, const std::string& key) {
        if (!obj.contains(key) || !obj[key].is_string() || obj[key].get<std::string>().empty())
            missing.push_back(key);
    };
    auto require_obj = [&](const std::string& key) {
        if (!cfg.contains(key) || !cfg[key].is_object())
            missing.push_back(key);
    };

    // main_model — LLM 对话必填
    require_obj("main_model");
    if (cfg.contains("main_model")) {
        const auto& m = cfg["main_model"];
        require_str(m, "url");
        require_str(m, "api_key");
        require_str(m, "model");
    }

    // summary_model — 历史压缩必填
    require_obj("summary_model");
    if (cfg.contains("summary_model")) {
        const auto& s = cfg["summary_model"];
        require_str(s, "url");
        require_str(s, "api_key");
        require_str(s, "model");
    }

    // napcat — QQ 机器人必填
    require_obj("napcat");
    if (cfg.contains("napcat")) {
        const auto& n = cfg["napcat"];
        require_str(n, "ws_url");
        require_str(n, "http_url");
        require_str(n, "token");
    }

    // embedding — 如独立配置则必填
    if (cfg.contains("embedding") && cfg["embedding"].is_object()) {
        const auto& e = cfg["embedding"];
        if (e.contains("api_url") || e.contains("api_key") || e.contains("model")) {
            require_str(e, "api_url");
            require_str(e, "api_key");
            require_str(e, "model");
        }
    }

    return missing;
}

// ── 创建模板 config.json ──
static bool create_template(const std::string& path) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        std::cerr << CLR_RED "[✗] 无法创建 " << path << "，请检查目录权限" CLR_RESET << std::endl;
        return false;
    }
    ofs << CONFIG_TEMPLATE;
    ofs.close();

    std::cout << CLR_YELLOW
        << "\n"
        << "╔══════════════════════════════════════════════════╗\n"
        << "║                                                  ║\n"
        << "║  首次运行：已创建 " << path << "\n"
        << "║                                                  ║\n"
        << "║  请编辑此文件，填入你的 API 配置后重新启动程序    ║\n"
        << "║                                                  ║\n"
        << "║  至少需要填写：                                  ║\n"
        << "║    • main_model.url / api_key / model            ║\n"
        << "║    • summary_model.url / api_key / model         ║\n"
        << "║    • search.url / api_key                        ║\n"
        << "║    • napcat.ws_url / http_url / token            ║\n"
        << "║                                                  ║\n"
        << "╚══════════════════════════════════════════════════╝\n"
        << CLR_RESET << std::endl;
    return true;
}

void Config::load(const std::string& path) {
    _lastPath = path;

    // ── 首次运行：创建模板并退出 ──
    if (!std::ifstream(path).good()) {
        std::cout << CLR_YELLOW "[!] 未找到 " << path << "，首次运行需要配置文件" CLR_RESET << std::endl;
        if (create_template(path)) {
            std::exit(1);
        } else {
            std::cerr << CLR_RED "[✗] 配置文件创建失败，程序退出" CLR_RESET << std::endl;
            std::exit(1);
        }
    }

    // ── 读取并解析 JSON ──
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << CLR_RED "[✗] 无法打开 " << path << CLR_RESET << std::endl;
        std::exit(1);
    }

    json cfg;
    try {
        ifs >> cfg;
    } catch (const std::exception& e) {
        std::cerr << CLR_RED "[✗] " << path << " JSON 解析失败: " << e.what() << CLR_RESET << std::endl;
        std::cerr << CLR_YELLOW "    请检查 JSON 格式是否正确（引号、逗号、括号等）" CLR_RESET << std::endl;
        std::exit(1);
    }

    if (!cfg.is_object()) {
        std::cerr << CLR_RED "[✗] " << path << " 根节点必须是 JSON 对象" CLR_RESET << std::endl;
        std::exit(1);
    }

    // ── 必填项验证 ──
    auto missing = validate_required(cfg);
    if (!missing.empty()) {
        std::cerr << CLR_RED "\n[✗] 配置文件缺少必填项：" << CLR_RESET << std::endl;
        for (const auto& field : missing) {
            std::cerr << CLR_RED "    • " << field << CLR_RESET << std::endl;
        }
        std::cerr << CLR_YELLOW "\n    请编辑 " << path << " 补充上述字段后重新启动\n" << CLR_RESET << std::endl;
        std::exit(1);
    }

    // ── 逐段解析（只读取 JSON 中存在的字段） ──
    if (cfg.contains("main_model") && cfg["main_model"].is_object())
        parse_model(cfg["main_model"], _main);

    if (cfg.contains("summary_model") && cfg["summary_model"].is_object())
        parse_model(cfg["summary_model"], _summary);

    if (cfg.contains("embedding") && cfg["embedding"].is_object()) {
        auto& e = cfg["embedding"];
        std::string v;
        v = json_str(e, "api_url");  if (!v.empty()) _embedding.api_url = v;
        v = json_str(e, "api_key");  if (!v.empty()) _embedding.api_key = v;
        v = json_str(e, "model");    if (!v.empty()) _embedding.model = v;
        v = json_str(e, "provider"); if (!v.empty()) _embedding.provider = v;
    }

    if (cfg.contains("ocr") && cfg["ocr"].is_object()) {
        auto& o = cfg["ocr"];
        _ocr.enabled = json_bool(o, "enabled", _ocr.enabled);
        std::string v;
        v = json_str(o, "provider"); if (!v.empty()) _ocr.provider = v;
        v = json_str(o, "model");    if (!v.empty()) _ocr.model = v;
        v = json_str(o, "url");      if (!v.empty()) _ocr.url = v;
        v = json_str(o, "api_key");  if (!v.empty()) _ocr.api_key = v;
        _ocr.max_images_per_message = json_int(o, "max_images_per_message", _ocr.max_images_per_message);
        if (_ocr.max_images_per_message < 1) _ocr.max_images_per_message = 1;
        _ocr.timeout_seconds = json_int(o, "timeout_seconds", _ocr.timeout_seconds);
        if (_ocr.timeout_seconds <= 0) _ocr.timeout_seconds = 45;
        if (_ocr.enabled && (_ocr.url.empty() || _ocr.api_key.empty() || _ocr.model.empty())) {
            _ocr.enabled = false;
        }
    }

    if (cfg.contains("napcat") && cfg["napcat"].is_object()) {
        auto& n = cfg["napcat"];
        std::string v;
        v = json_str(n, "ws_url");   if (!v.empty()) _napcat.ws_url = v;
        v = json_str(n, "http_url"); if (!v.empty()) _napcat.http_url = v;
        v = json_str(n, "token");    if (!v.empty()) _napcat.token = v;
        if (n.contains("group_whitelist") && n["group_whitelist"].is_array()) {
            _napcat.group_whitelist.clear();
            for (auto& id : n["group_whitelist"])
                if (id.is_number_integer()) _napcat.group_whitelist.insert(id.get<int64_t>());
        }
        if (n.contains("private_whitelist") && n["private_whitelist"].is_array()) {
            _napcat.private_whitelist.clear();
            for (auto& id : n["private_whitelist"])
                if (id.is_number_integer()) _napcat.private_whitelist.insert(id.get<int64_t>());
        }
        if (n.contains("admin_users") && n["admin_users"].is_array()) {
            _napcat.admin_users.clear();
            for (auto& u : n["admin_users"])
                if (u.is_string()) _napcat.admin_users.insert(u.get<std::string>());
        }
        if (n.contains("bot_aliases") && n["bot_aliases"].is_array()) {
            _napcat.bot_aliases.clear();
            for (auto& a : n["bot_aliases"])
                if (a.is_string()) _napcat.bot_aliases.push_back(a.get<std::string>());
        }
        v = json_str(n, "identity_rules_suffix"); if (!v.empty()) _napcat.identity_rules_suffix = v;
    }

    std::string sp = json_str(cfg, "system_prompt");
    if (!sp.empty()) _sys_prompt = sp;

    // 消息管线配置
    if (cfg.contains("pipeline") && cfg["pipeline"].is_object()) {
        auto& p = cfg["pipeline"];
        double d = json_num(p, "silentChance", -1);
        if (d >= 0 && d <= 1) {
            _pipeline.silentChance = d;
            _pipeline.unmentionedEvalChance = d;
        }
        double evalChance = json_num(p, "unmentionedEvalChance", -1);
        if (evalChance >= 0 && evalChance <= 1) _pipeline.unmentionedEvalChance = evalChance;
        _pipeline.idleCompensationSeconds = json_int(p, "idleCompensationSeconds", _pipeline.idleCompensationSeconds);
        if (_pipeline.idleCompensationSeconds < 0) _pipeline.idleCompensationSeconds = 0;
        _pipeline.idleQuietStartHour = json_int(p, "idleQuietStartHour", _pipeline.idleQuietStartHour);
        if (_pipeline.idleQuietStartHour < 0 || _pipeline.idleQuietStartHour > 23) _pipeline.idleQuietStartHour = 0;
        _pipeline.idleQuietEndHour = json_int(p, "idleQuietEndHour", _pipeline.idleQuietEndHour);
        if (_pipeline.idleQuietEndHour < 0 || _pipeline.idleQuietEndHour > 23) _pipeline.idleQuietEndHour = 7;
        _pipeline.cooldownSeconds = json_int(p, "cooldownSeconds", 30);
        if (_pipeline.cooldownSeconds < 0) _pipeline.cooldownSeconds = 30;
        _pipeline.logLevel = json_str(p, "logLevel", _pipeline.logLevel);
    }

    // 屏蔽词配置
    if (cfg.contains("ban_words") && cfg["ban_words"].is_array()) {
        for (auto& w : cfg["ban_words"])
            if (w.is_string()) _ban.ban_words.push_back(w.get<std::string>());
    }
    if (cfg.contains("ban_regex") && cfg["ban_regex"].is_array()) {
        for (auto& r : cfg["ban_regex"])
            if (r.is_string()) _ban.ban_regex.push_back(r.get<std::string>());
    }
    if (cfg.contains("ad_keywords") && cfg["ad_keywords"].is_array()) {
        for (auto& kw : cfg["ad_keywords"])
            if (kw.is_string()) _ban.ad_keywords.push_back(kw.get<std::string>());
    }

    // 命令配置
    if (cfg.contains("commands") && cfg["commands"].is_array()) {
        for (auto& c : cfg["commands"]) {
            if (c.is_object()) {
                CommandsConfig::Entry e;
                e.prefix  = json_str(c, "prefix");
                e.name    = json_str(c, "name");
                e.description = json_str(c, "description", "");
                if (!e.name.empty()) _commands.commands.push_back(e);
            }
        }
    }

    // A_Memorix 长期记忆配置
    if (cfg.contains("a_memorix") && cfg["a_memorix"].is_object()) {
        auto& a = cfg["a_memorix"];
        _a_memorix.enabled = json_bool(a, "enabled", _a_memorix.enabled);
        std::string dir = json_str(a, "data_dir");
        if (!dir.empty()) _a_memorix.data_dir = dir;

        if (a.contains("integration") && a["integration"].is_object()) {
            auto& i = a["integration"];
            _a_memorix.enable_query = json_bool(i, "enable_query", _a_memorix.enable_query);
            _a_memorix.inject_profile = json_bool(i, "inject_profile", _a_memorix.inject_profile);
            _a_memorix.write_person_facts = json_bool(i, "write_person_facts", _a_memorix.write_person_facts);
            _a_memorix.write_chat_summary = json_bool(i, "write_chat_summary", _a_memorix.write_chat_summary);
            _a_memorix.feedback_correction = json_bool(i, "feedback_correction", _a_memorix.feedback_correction);
        }
        if (a.contains("writeback") && a["writeback"].is_object()) {
            auto& w = a["writeback"];
            _a_memorix.after_reply = json_bool(w, "after_reply", _a_memorix.after_reply);
            _a_memorix.message_window = json_int(w, "message_window", _a_memorix.message_window);
            if (_a_memorix.message_window <= 0) _a_memorix.message_window = 30;
            double importance = json_num(w, "min_importance", _a_memorix.min_importance);
            if (importance >= 0 && importance <= 1) _a_memorix.min_importance = importance;
            _a_memorix.summary_interval_minutes = json_int(w, "summary_interval_minutes", _a_memorix.summary_interval_minutes);
            if (_a_memorix.summary_interval_minutes <= 0) _a_memorix.summary_interval_minutes = 30;
        }
        if (a.contains("retrieval") && a["retrieval"].is_object()) {
            auto& r = a["retrieval"];
            _a_memorix.max_facts = json_int(r, "max_facts", _a_memorix.max_facts);
            if (_a_memorix.max_facts < 0) _a_memorix.max_facts = 0;
            _a_memorix.max_episodes = json_int(r, "max_episodes", _a_memorix.max_episodes);
            if (_a_memorix.max_episodes < 0) _a_memorix.max_episodes = 0;
            _a_memorix.max_profile_chars = json_int(r, "max_profile_chars", _a_memorix.max_profile_chars);
            if (_a_memorix.max_profile_chars < 0) _a_memorix.max_profile_chars = 0;
        }
    }

    // MCP Server 配置
    if (cfg.contains("mcp_servers") && cfg["mcp_servers"].is_array()) {
        _mcp_servers.clear();
        for (auto& s : cfg["mcp_servers"]) {
            if (!s.is_object()) continue;
            McpServerEntry e;
            e.name = json_str(s, "name");
            e.url = json_str(s, "url");
            e.transport = json_str(s, "transport", "streamable_http");
            e.api_key = json_str(s, "api_key");
            if (s.contains("headers") && s["headers"].is_object()) {
                for (auto& [k, v] : s["headers"].items()) {
                    if (v.is_string()) e.headers[k] = v.get<std::string>();
                }
            }
            e.enabled = json_bool(s, "enabled", true);
            if (!e.name.empty() && !e.url.empty())
                _mcp_servers.push_back(std::move(e));
        }
    }

    std::cout << CLR_GREEN "[✓] 配置已加载: " << path << CLR_RESET << std::endl;
}
