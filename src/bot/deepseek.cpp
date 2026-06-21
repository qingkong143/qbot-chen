#include "src/bot/deepseek.h"
#include "src/core/config.h"

size_t Deepseek::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

//发送请求（全部参数从 Config 读取）
json Deepseek::SendChatCompletion(CURL* curl, const json& messages, const json& tools,
    const std::string& tool_choice) {
    if (!curl) return json::object();

    const auto& m = Config::get().main_model();

    std::string responseBody;

    json requestBody = {
        {"model", m.model},
        {"messages", messages},
        {"tools", tools},
        {"tool_choice", tool_choice},
        {"temperature", m.temperature},
        {"max_tokens", m.max_tokens}
    };
    std::string bodyString = requestBody.dump();

    // 调试：打印 max_tokens 实际值
    std::cout << "[DEBUG] max_tokens=" << m.max_tokens << ", model=" << m.model << std::endl;

    curl_easy_setopt(curl, CURLOPT_URL, m.url.c_str());
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + m.api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyString.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Deepseek::WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        std::cerr << CLR_RED "[✗] curl: " << curl_easy_strerror(res) << CLR_RESET << std::endl;
        return json::object();
    }

    try {
        // API 返回的 JSON 始终是合法 UTF-8，直接解析即可
        const std::string& parseBody = responseBody;
        json response = json::parse(parseBody, nullptr, false);
        if (response.is_null() || response.is_discarded()) {
            std::cerr << CLR_RED "[✗] JSON parse failed, body size: "
                << responseBody.size() << CLR_RESET << std::endl;
            return json::object();
        }

        if (response.contains("error")) {
            std::string message = response["error"].is_object()
                ? response["error"].value("message", response["error"].dump())
                : response["error"].dump();
            std::cerr << CLR_RED "[✗] API: " << message << CLR_RESET << std::endl;
            return json::object();
        }
        if (http_code < 200 || http_code >= 300) {
            std::cerr << CLR_RED "[✗] HTTP " << http_code << ": "
                << responseBody.substr(0, 200) << CLR_RESET << std::endl;
            return json::object();
        }
        return response;
    }
    catch (const std::exception& e) {
        std::cerr << CLR_RED "[✗] JSON error: " << e.what() << CLR_RESET << std::endl;
        return json::object();
    }
}
