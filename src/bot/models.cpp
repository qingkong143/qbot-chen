#include "src/bot/models.h"
#include "src/core/config.h"
#include <curl/curl.h>

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    output->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::vector<double> Models::getEmbedding(CURL* curl, const std::string& text) {
    std::vector<double> result;
    if (!curl || text.empty()) return result;

    // 使用 DeepSeek embedding 模型（若支持）或用 chat API 的 embedding 端点
    // 这里演示用 DeepSeek 官方的 embedding API（如果有）
    // 若无，可降级到用 chat API 的隐式 embedding

    const auto& modelCfg = Config::get().main_model();
    std::string embeddingUrl = modelCfg.url;
    // 替换为 embedding 端点（假设格式为 /v1/embeddings）
    size_t pos = embeddingUrl.find("/chat/completions");
    if (pos != std::string::npos) {
        embeddingUrl = embeddingUrl.substr(0, pos) + "/embeddings";
    }

    json requestBody;
    requestBody["model"] = "text-embedding-3-small";  // 或其他模型
    requestBody["input"] = text;

    std::string jsonStr = requestBody.dump();
    std::string response;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string authHeader = "Authorization: Bearer " + modelCfg.api_key;
    headers = curl_slist_append(headers, authHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, embeddingUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        std::cerr << "[Embedding] curl 请求失败: " << curl_easy_strerror(res) << std::endl;
        return result;
    }

    try {
        json responseJson = json::parse(response);
        if (responseJson.contains("data") && responseJson["data"].is_array() && !responseJson["data"].empty()) {
            if (responseJson["data"][0].contains("embedding")) {
                auto& embedding = responseJson["data"][0]["embedding"];
                if (embedding.is_array()) {
                    for (const auto& val : embedding) {
                        result.push_back(val.get<double>());
                    }
                }
            }
        }
    } catch (...) {
        std::cerr << "[Embedding] JSON 解析失败: " << response << std::endl;
    }

    return result;
}
