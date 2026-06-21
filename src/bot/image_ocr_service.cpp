#include "src/bot/image_ocr_service.h"
#include "src/core/config.h"
#include "src/infra/logger.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <cstdio>

using json = nlohmann::json;

ImageOcrService& ImageOcrService::get() {
    static ImageOcrService instance;
    return instance;
}

size_t ImageOcrService::writeCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t total = size * nmemb;
    output->append(static_cast<char*>(contents), total);
    return total;
}

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::vector<std::string> ImageOcrService::extractImageUrls(const std::string& raw_message, int max_images) const {
    std::vector<std::string> urls;
    if (max_images <= 0) return urls;

    size_t pos = 0;
    while (urls.size() < static_cast<size_t>(max_images)) {
        size_t start = raw_message.find("[CQ:image", pos);
        if (start == std::string::npos) break;

        size_t end = raw_message.find(']', start);
        if (end == std::string::npos) break;

        std::string cq = raw_message.substr(start, end - start + 1);
        size_t url_pos = cq.find("url=");
        if (url_pos != std::string::npos) {
            url_pos += 4;
            size_t url_end = cq.find(',', url_pos);
            if (url_end == std::string::npos) url_end = cq.find(']', url_pos);
            if (url_end != std::string::npos && url_end > url_pos) {
                std::string url = cq.substr(url_pos, url_end - url_pos);
                replace_all(url, "&amp;", "&");
                replace_all(url, "&#91;", "[");
                replace_all(url, "&#93;", "]");
                if (!url.empty()) urls.push_back(url);
            }
        }

        pos = end + 1;
    }

    return urls;
}

std::vector<std::string> ImageOcrService::extractImageFiles(const std::string& raw_message, int max_images) const {
    std::vector<std::string> files;
    if (max_images <= 0) return files;

    size_t pos = 0;
    while (files.size() < static_cast<size_t>(max_images)) {
        size_t start = raw_message.find("[CQ:image", pos);
        if (start == std::string::npos) break;

        size_t end = raw_message.find(']', start);
        if (end == std::string::npos) break;

        std::string cq = raw_message.substr(start, end - start + 1);
        size_t file_pos = cq.find("file=");
        if (file_pos != std::string::npos) {
            file_pos += 5;
            size_t file_end = cq.find(',', file_pos);
            if (file_end == std::string::npos) file_end = cq.find(']', file_pos);
            if (file_end != std::string::npos && file_end > file_pos) {
                std::string file_id = cq.substr(file_pos, file_end - file_pos);
                replace_all(file_id, "&amp;", "&");
                replace_all(file_id, "&#91;", "[");
                replace_all(file_id, "&#93;", "]");
                if (!file_id.empty()) files.push_back(file_id);
            }
        }

        pos = end + 1;
    }

    return files;
}

std::string ImageOcrService::fetchImageUrlFromNapcat(const std::string& file_id) const {
    const auto& nc = Config::get().napcat();
    if (nc.http_url.empty() || nc.token.empty() || file_id.empty()) return "";

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response_body;
    std::string url = nc.http_url + "/get_image";

    json body;
    body["file"] = file_id;
    std::string request_body = body.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + nc.token;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ImageOcrService::writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300 || response_body.empty()) {
        return "";
    }

    json resp = json::parse(response_body, nullptr, false);
    if (resp.is_discarded() || !resp.is_object()) return "";
    if (!resp.contains("data") || !resp["data"].is_object()) return "";

    const auto& data = resp["data"];
    if (data.contains("url") && data["url"].is_string()) {
        return data["url"].get<std::string>();
    }

    return "";
}


std::string ImageOcrService::resolveImageSource(const std::string& image_ref) const {
    if (image_ref.empty()) return "";

    if (image_ref.rfind("base64://", 0) == 0) {
        Logger::get().warn("[图片识别]", "收到 base64 图片引用，已跳过转码，建议直接提供可访问 URL");
        return "";
    }

    std::string image_url = fetchImageUrlFromNapcat(image_ref);
    if (!image_url.empty()) return image_url;

    if (image_ref.rfind("http://", 0) == 0 || image_ref.rfind("https://", 0) == 0) {
        return image_ref;
    }

    return "";
}

std::string ImageOcrService::recognizeImages(const std::vector<std::string>& image_refs) const {
    const auto& cfg = Config::get().ocr();
    if (!cfg.enabled || image_refs.empty()) return "";

    std::ostringstream oss;
    bool first = true;
    for (size_t i = 0; i < image_refs.size(); ++i) {
        std::string text = recognizeOne(image_refs[i], static_cast<int>(i + 1));
        if (text.empty()) continue;
        if (first) {
            oss << "[OCR图片识别结果（仅供参考）]\n";
            first = false;
        }
        oss << "- 第" << (i + 1) << "张：" << text << "\n";
    }

    return oss.str();
}

std::string ImageOcrService::recognizeOne(const std::string& image_ref, int index) const {
    const auto& cfg = Config::get().ocr();
    if (!cfg.enabled || image_ref.empty()) return "";
    if (cfg.url.empty() || cfg.api_key.empty() || cfg.model.empty()) {
        Logger::get().warn("[图片识别]", "OCR 配置不完整，跳过图片识别");
        return "";
    }

    std::string image_url = resolveImageSource(image_ref);
    if (image_url.empty()) {
        Logger::get().warn("[图片识别]", "图片引用无法解析为可访问 URL");
        return "";
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::get().warn("[图片识别]", "curl 初始化失败");
        return "";
    }

    json content = json::array();
    content.push_back({
        {"type", "text"},
        {"text", "请对图片做OCR逐字识别，只输出图片中的文字内容。不要总结，不要解释，不要猜测，不要补全，不要客套。尽量保留原始换行、数字、标点和表格结构；如果有看不清的字，用□标记。"}
    });
    content.push_back({
        {"type", "image_url"},
        {"image_url", {{"url", image_url}}}
    });

    json body;
    body["model"] = cfg.model;
    body["messages"] = json::array({{{"role", "user"}, {"content", content}}});
    body["temperature"] = 0.1;
    body["max_tokens"] = 2048;

    std::string request_body = body.dump();
    std::string response_body;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + cfg.api_key;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, cfg.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ImageOcrService::writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    Logger::get().info("[图片识别]", "开始识别第 " + std::to_string(index) + " 张图片");
    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        Logger::get().warn("[图片识别]", "OCR 请求失败: " + std::string(curl_easy_strerror(res)));
        return "";
    }
    if (http_code < 200 || http_code >= 300) {
        Logger::get().warn("[图片识别]", "OCR HTTP 状态异常: " + std::to_string(http_code));
        if (!response_body.empty()) {
            Logger::get().warn("[图片识别]", "OCR 响应体: " + response_body.substr(0, 500));
        }
        return "";
    }
    if (response_body.empty()) {
        Logger::get().warn("[图片识别]", "OCR API 响应为空");
        return "";
    }

    json response = json::parse(response_body, nullptr, false);
    if (response.is_discarded() || !response.is_object()) {
        Logger::get().warn("[图片识别]", "OCR 响应 JSON 解析失败");
        return "";
    }

    try {
        if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
            const auto& msg = response["choices"][0]["message"];
            if (msg.contains("content")) {
                if (msg["content"].is_string()) {
                    std::string text = msg["content"].get<std::string>();
                    Logger::get().info("[图片识别]", "OCR 原始返回: " + text.substr(0, 500));
                    return text;
                }
                if (msg["content"].is_array()) {
                    std::ostringstream out;
                    for (const auto& part : msg["content"]) {
                        if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                            out << part["text"].get<std::string>() << "\n";
                        }
                    }
                    return out.str();
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::get().warn("[图片识别]", "OCR 响应提取失败: " + std::string(e.what()));
        return "";
    }

    Logger::get().warn("[图片识别]", "OCR 响应中没有可用内容");
    return "";
}
