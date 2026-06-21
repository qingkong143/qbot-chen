#pragma once
#include "src/core/base.h"
#include "src/bot/models.h"
#include "src/core/tools.h"
#include "src/memory/jargon_miner.h"
#include "src/infra/reply_optimizer.h"
#include "src/infra/message_deduplicator.h"
#include <functional>
#include <set>
#include <string>
#include <atomic>
#include <mutex>

using json = nlohmann::json;

class agent {
public:
	~agent();

	// 初始化：分配 curl、写入 system prompt（非交互模式必须先调用）
	void init();

	// 设置行话挖掘器的 LLM 依赖
	void initJargonMiner() {
		JargonMiner::get().setLLMDependencies(&models.deepseek, curl);
	}

	// 交互式控制台模式（自己读 cin、写 cout）
	void run();

	// 非交互式查询：输入用户消息，返回 AI 文本回复（可能触发 tool calls）
	std::string query(const std::string& user_message);
	std::string queryWithEphemeralContext(const std::string& user_message,
		const std::string& ephemeral_context);
	bool isReasoning() const { return _is_reasoning.load(); }
	void injectTurnMessage(const std::string& content, const std::string& sender_id = "");

	// 输出回调：NapCat 模式下转发进度/工具调用信息到 QQ
	using OutputCallback = std::function<void(const std::string&)>;
	void setOutputCallback(OutputCallback cb);

	// 注入历史上下文（重启后恢复记忆，不触发 API 调用）
	void injectHistory(const std::string& historyText);
	// 压缩后再注入（先经 summary model 压缩，避免浪费上下文窗口）
	void injectHistoryCompressed(const std::string& historyText);

	// 非交互模式权限控制：限制 exec_cmd 仅白名单用户可用
	void setRestrictedMode(bool restricted);
	void setAllowedExecUsers(const std::set<std::string>& users);
	void setCurrentUser(const std::string& user_id);

	// 群风格相关
	void setChatId(int64_t id, const std::string& type); // 设置会话 ID 供自动查风格
	void setSpeaker(int64_t user_id, const std::string& name); // 设置当前发言人供长期记忆检索
	void injectGroupStyle(const std::string& styleText); // 手动注入群风格

	Tools tools;
	Models models;
	json summarizeHistory(json& old_messages);
	static json msgSizeCheck(json& msgs, int total_token);
	json GetContent(const json& response, json& messages);

	// 获取 curl 句柄（供外部直接调 API 时复用）
	CURL* getCurlHandle() { return curl; }

	// 消息去重接口
	bool shouldProcessMessage(const std::string& message, const std::string& user_id = "") {
		return _deduplicator.shouldProcess(message, user_id);
	}
	void addMessageToHistory(const std::string& message, const std::string& user_id = "") {
		_deduplicator.addMessage(message, user_id);
	}

private:
	json messages;          // 对话历史（跨轮次保留）
	CURL* curl = nullptr;   // 持久化 curl 句柄
	// su_url / su_key / sys_prompt 已迁移到 Config 单例
	OutputCallback _onOutput;
	MessageDeduplicator _deduplicator;  // 消息去重模块

	static void printSep(char ch = '-');
	static void initDisplay();
	static std::string utf8SafeSubstr(const std::string& str, int maxLen);
	json summarizeModel(const std::string& prompt, int maxPromptChars = -1);
	static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output);
	static int estimateToken(const json& msg);
	static int estimateTokens(const json& msgs);
	std::string summarizeToolResult(const std::string& toolName, const std::string& rawContent);
	static json truncateMsgContent(json msg, int maxChars);

	// 权限控制
	bool _restrictedMode = false;
	std::set<std::string> _allowedExecUsers;
	std::string _currentUserId;

	// 群风格
	std::string _groupStyle;
	int64_t _currentChatId = 0;
	std::string _currentChatType;
	int64_t _currentSpeakerId = 0;
	std::string _currentSpeakerName;

	// 根据权限返回可用工具列表（受限模式下过滤 exec_cmd）
	json getAvailableTools() const;

	// 内部：处理一轮完整的 tool-call 循环，返回最终 AI 文本
	std::string processQuery(const std::string& api_user_content = "",
		const std::string& stored_user_content = "");

	// Phase 2: Reasoning Engine
	struct TurnMessage {
		std::string content;
		int64_t timestamp;
		std::string sender_id;
	};
	std::vector<TurnMessage> _turn_queue;
	std::mutex _turnQueueMutex;
	int _max_reasoning_turns = 5;
	std::atomic<bool> _is_reasoning{false};

	// 多轮推理循环：处理队列中的消息，支持中断注入
	std::string run_loop(const std::string& initial_message);
};