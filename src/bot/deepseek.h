#pragma once
#include "src/core/base.h"

using json = nlohmann::json;

class Deepseek {
public:
	//发送请求（参数从 Config 单例读取）；tool_choice 默认 "auto"，传 "none" 强制文本回复
	json SendChatCompletion(CURL* curl, const json& messages, const json& tools,
		const std::string& tool_choice = "auto");
private:
	static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output);
};