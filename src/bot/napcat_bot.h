#pragma once
#ifdef _WIN32
    #include <winsock2.h>
#endif
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include "src/bot/agent.h"
#include "src/core/command.h"
#include "src/memory/jargon_miner.h"
#include "src/infra/event_bus.h"
#include "src/knowledge/embedding_service.h"
#include "src/knowledge/embedding_store.h"
#include "src/knowledge/knowledge_retriever.h"
#include "src/infra/cleanup_manager.h"
#include "src/knowledge/quality_scorer.h"
#include "src/knowledge/knowledge_sharing.h"
#include "src/infra/database_migrator.h"
#include "src/mcp/mcp_manager.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <set>

using json = nlohmann::json;

class Napcat {
public:
	using ApiCallback = std::function<void(const json& response)>;

	Napcat();
	void run();

	// 通用 API 调用
	void callApi(const std::string& action, const json& params, ApiCallback callback);

	// 便捷 API
	std::string getLoginInfo();
	void getGroupList(ApiCallback callback);
	void getGroupMemberList(int64_t group_id, ApiCallback callback);
	void getGroupInfo(int64_t group_id, ApiCallback callback);
	void getFriendList(ApiCallback callback);
	void getStatus(ApiCallback callback);
	void getGroupMsgHistory(int64_t group_id, ApiCallback callback);
	void getFriendMsgHistory(int64_t user_id, ApiCallback callback);

	// 发送消息（fire-and-forget）
	void sendLike(int64_t user_id);
	void sendGroupMsg(int64_t group_id, const std::string& message);
	void sendPrivateMsg(int64_t user_id, const std::string& message);

	// 快速操作（基于当前事件 context 的回复/管理动作）
	void handleQuickOperation(const json& context, const json& operation);
	void quickReply(const json& context, const std::string& text,
		bool atSender = false, bool autoEscape = false);
	void quickReplyLong(const json& context, const std::string& text,
		bool atSender = false, bool autoEscape = false);
	void quickDelete(const json& context);
	void quickKick(const json& context);
	void quickBan(const json& context, int64_t durationSeconds);
	void quickApprove(const json& context, const std::string& remark = "");
	void quickReject(const json& context, const std::string& reason = "");
	void quickSetRemark(const json& context, const std::string& remark);

private:
	std::string NAPCAT_WS_URL;
	std::string NAPCAT_HTTP_URL;
	std::string _token;

	ix::WebSocket _ws;
	std::atomic<int64_t> _echoCounter{0};
	std::map<std::string, ApiCallback> _pendingCallbacks;
	std::mutex _callbackMutex;

	// 群聊和私聊上下文隔离
	agent _agentGroup;                                 
	std::unordered_map<int64_t, std::unique_ptr<agent>> _privateAgents; // 私聊每人独立
	bool _groupAgentReady = false;

	// 发送间隔控制（防风控）
	std::chrono::steady_clock::time_point _lastSendTime{};
	static constexpr std::chrono::milliseconds SEND_INTERVAL{1000};
	void throttleSend();  // 在上次发送未满间隔时等待

	// 获取对应 agent（群聊取共享，私聊按 user_id 取/创建独立实例）
	agent& getAgent(int64_t user_id, bool isGroup);

	// 历史恢复标记（防止重复注入）
	bool _groupHistoryLoaded = false;
	std::set<int64_t> _privateHistoryLoaded;
	void loadGroupHistory();
	void loadPrivateHistory(int64_t user_id);

	// Bot 自己的 QQ 号（用于 @ 检测）
	std::string _selfId;

	// 冷却时间控制
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> _lastAiReplyTimes;
	mutable std::mutex _replyTimeMutex;  // 保护 _lastAiReplyTimes 并发访问

	// @ 消息去重（检测骚扰式重复 @）
	struct AtMessageRecord {
		int64_t user_id;
		std::string message_hash;
		std::chrono::steady_clock::time_point timestamp;
	};
	std::vector<AtMessageRecord> _atMessageHistory;
	static constexpr int AT_MESSAGE_HISTORY_MAX = 20;
	static constexpr int AT_SPAM_WINDOW_MS = 5000;
	static constexpr int AT_SPAM_THRESHOLD = 3;
	bool isAtSpamming(int64_t user_id, const std::string& message_content);

	// 空闲补偿（群内长时间无消息时主动开启话题）
	struct GroupIdleState {
		int64_t group_id;
		std::chrono::steady_clock::time_point lastActivityTime;
		bool hasTriggeredIdle = false;
	};
	std::unordered_map<int64_t, GroupIdleState> _groupIdleStates;
	static constexpr int IDLE_THRESHOLD_SECONDS = 300;  // 5分钟
	void checkAndTriggerIdleCompensation(int64_t group_id);

	// 命令处理器
	CommandHandler _cmdHandler;

	// MCP 管理器
	McpManager _mcpManager;

	// 当前正在处理的会话上下文
	int64_t _currentGroupId = 0;
	int64_t _currentUserId = 0;
	int64_t _currentMessageId = 0;
	std::string _currentMsgType;  // "group" / "private"

	void registerBotTools(class Tools& tools);
	void sendAction(const std::string& action, const json& params);
	bool tryHandleApiResponse(const json& data);
	void handleMessage(const json& data);

	static std::string sanitizeUtf8(const std::string& input);
	static std::string utf8SafeTruncate(const std::string& input, size_t maxBytes);

	// 检测消息是否 @了机器人
	bool isAtBot(const std::string& raw_message) const;

	// 消息管线：屏蔽词检查
	bool checkBanWords(const std::string& text) const;

	// 消息管线：命令匹配
	bool checkCommand(const json& data, const std::string& text,
		const json& context);

	// 消息管线：AI 评估是否应该插话（未 @ 的群消息）
	enum class ReplyDecision { Silence, Cooldown, Reply };
	enum class TimingGateDecision { Continue, NoAction, Wait };
	TimingGateDecision evaluateTimingGate(const std::string& raw_message,
		int64_t group_id, const std::string& recent_context, agent& currentAgent);
	ReplyDecision evaluateShouldReply(const std::string& raw_message,
		int64_t group_id, const std::string& recent_context, agent& currentAgent);

	// 冷却与空闲补偿
	bool shouldUseIdleCompensation(const std::string& sessionKey, int idleSeconds) const;
	void recordAiReplyTime(const std::string& sessionKey);
};