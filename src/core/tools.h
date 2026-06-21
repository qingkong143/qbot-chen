#pragma once

#include "src/core/base.h"
#include "src/core/math.h"
#include <unordered_map>

using json = nlohmann::json;

class Tools
{
public:
	// 外部工具处理器：NapCat 等模块可通过此回调向 AI 暴露自身能力
	using ToolHandler = std::function<std::string(const json& args)>;

	Tools();
	json GetTools() const;
	void ProcessToolCalls(const json& toolCalls, json& toolResponses);
	static std::string ListDirectory(const std::string& root_path, bool recursive);
	std::string exec_cmd(const std::string& cmd);
	static std::string get_current_time();

	// 注册外部工具（工具名 + JSON定义 + 处理回调）
	void registerTool(const std::string& name, const json& definition, ToolHandler handler);

private:
	struct RegisteredTool {
		json definition;
		ToolHandler handler;
	};
	std::unordered_map<std::string, RegisteredTool> _externalTools;

	static void TraverseDirectory(const std::wstring& dirPath, std::ostringstream& oss,
		bool recursive, int& count, int maxEntries);
	static std::string Utf16ToUtf8(const std::wstring& utf16);
	static std::wstring Utf8ToUtf16(const std::string& utf8);
	static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output);
};