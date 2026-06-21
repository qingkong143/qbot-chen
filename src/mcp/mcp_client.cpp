#include "src/mcp/mcp_client.h"
#include "src/core/config.h"
#include "src/bot/deepseek.h"
#include <curl/curl.h>
#include <iostream>

using json = nlohmann::json;

// ── curl write callback ──────────────────────────────────────────
static size_t McpWriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// ── 构造 ──────────────────────────────────────────────────────────
McpClient::McpClient(McpServerConfig cfg) : _cfg(std::move(cfg)) {}

McpClient::~McpClient() = default;

// ── HTTP POST ─────────────────────────────────────────────────────
std::string McpClient::httpPost(const std::string& /*path*/, const json& body) {
    // streamable_http: 直接 POST 到 URL
    std::string url = _cfg.url;
    std::string bodyStr = body.dump();
    std::cout << CLR_CYAN "[MCP] POST " << url << " body=" << bodyStr.substr(0, 200) << CLR_RESET << std::endl;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << CLR_RED "[MCP] curl_easy_init failed for " << _cfg.name << CLR_RESET << std::endl;
        return "";
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_slist_append(headers, "Accept: application/json, text/event-stream");

    // 自定义 headers（优先于 api_key）
    for (const auto& [k, v] : _cfg.headers) {
        if (!v.empty())
            headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }

    // 如果 headers 里没有 Authorization 且 api_key 非空，自动添加
    if (!_cfg.api_key.empty() && _cfg.headers.count("Authorization") == 0) {
        headers = curl_slist_append(headers, ("Authorization: Bearer " + _cfg.api_key).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, McpWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << CLR_RED "[MCP] HTTP error for " << _cfg.name
            << ": " << curl_easy_strerror(res) << CLR_RESET << std::endl;
        return "";
    }
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    std::cout << CLR_CYAN "[MCP] HTTP " << http_code << " body=" << responseBody.substr(0, 500) << CLR_RESET << std::endl;
    return responseBody;
}

// ── JSON-RPC 请求 ─────────────────────────────────────────────────
json McpClient::sendRequest(const std::string& method, const json& params) {
    std::lock_guard<std::mutex> lock(_mutex);

    json req = {
        {"jsonrpc", "2.0"},
        {"id", _requestId++},
        {"method", method},
        {"params", params}
    };

    std::string raw = httpPost("", req);

    if (raw.empty()) return json::object();

    // 处理 SSE：可能有多条事件，取最后一条 data
    if (raw.find("data:") != std::string::npos) {
        std::string lastData;
        size_t pos = 0;
        while (true) {
            auto p = raw.find("data:", pos);
            if (p == std::string::npos) break;
            p += 5;
            while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t')) ++p;
            size_t end = raw.find('\n', p);
            if (end == std::string::npos) end = raw.size();
            lastData = raw.substr(p, end - p);
            pos = end + 1;
        }
        raw = lastData;
    }

    json resp;
    try {
        resp = json::parse(raw, nullptr, false);
    } catch (...) {
        return json::object();
    }

    if (resp.contains("error") && !resp["error"].is_null()) {
        std::cerr << CLR_RED "[MCP] " << _cfg.name << " RPC error: "
            << resp["error"].value("message", resp["error"].dump()) << CLR_RESET << std::endl;
        return json::object();
    }

    return resp;
}

// ── 初始化握手 ────────────────────────────────────────────────────
bool McpClient::initialize() {
    json initParams = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "qbot"}, {"version", "1.0"}}}
    };

    json resp = sendRequest("initialize", initParams);
    if (resp.is_null() || resp.is_discarded()) {
        std::cerr << CLR_RED "[MCP] " << _cfg.name << " 初始化失败" << CLR_RESET << std::endl;
        return false;
    }

    // 发送 initialized 通知（不需要 response）
    sendRequest("notifications/initialized", json::object());

    _initialized = true;
    std::cout << CLR_GREEN "[MCP] " << _cfg.name << " 已连接" << CLR_RESET << std::endl;
    return true;
}

// ── 列出工具 ──────────────────────────────────────────────────────
std::vector<McpTool> McpClient::listTools() {
    json resp = sendRequest("tools/list", json::object());
    std::cout << CLR_CYAN "[MCP] tools/list raw resp: " << resp.dump(2).substr(0, 500) << CLR_RESET << std::endl;
    if (resp.is_null() || !resp.contains("result") || !resp["result"].contains("tools")) {
        return {};
    }

    std::vector<McpTool> tools;
    for (const auto& t : resp["result"]["tools"]) {
        McpTool tool;
        tool.name = t.value("name", "");
        tool.description = t.value("description", "");
        tool.parameters = t.value("inputSchema", json::object());
        tool.server = _cfg.name;
        if (!tool.name.empty()) tools.push_back(std::move(tool));
    }
    return tools;
}

// ── 调用工具 ──────────────────────────────────────────────────────
std::string McpClient::callTool(const std::string& toolName, const json& args) {
    json params = {
        {"name", toolName},
        {"arguments", args}
    };

    json resp = sendRequest("tools/call", params);
    if (resp.is_null() || !resp.contains("result")) {
        return "[MCP 调用失败]";
    }

    // 提取 text content
    const auto& content = resp["result"]["content"];
    if (content.is_array()) {
        std::string result;
        for (const auto& item : content) {
            if (item.value("type", "") == "text") {
                result += item.value("text", "");
            }
        }
        return result.empty() ? "[MCP 返回空结果]" : result;
    }

    if (content.is_object() && content.contains("text")) {
        return content["text"];
    }

    return content.dump();
}
