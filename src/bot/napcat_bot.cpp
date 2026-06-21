#include "src/bot/napcat_bot.h"
#include "src/core/config.h"
#include "src/memory/memory.h"
#include "src/core/command.h"
#include "src/memory/style_cache.h"
#include "src/memory/style_learner.h"
#include "src/memory/long_memory.h"
#include "src/infra/logger.h"
#include "src/infra/event_bus.h"
#include "src/plugin/plugin_loader.h"
#include "src/memory/memory_manager.h"
#include "src/bot/connection_pool.h"
#include "src/memory/vocabulary_manager.h"
#include "src/bot/image_ocr_service.h"
#include <limits>

using json = nlohmann::json;

static json g_currentMessageContext;

namespace {
static bool isUtf8ContinuationByte(unsigned char c);
static bool isUtf8ContinuationByte(unsigned char c) {
	return (c & 0xC0) == 0x80;
}

std::string sanitizeUtf8String(const std::string& input) {
	std::string out;
	out.reserve(input.size());

	for (size_t i = 0; i < input.size();) {
		unsigned char c = static_cast<unsigned char>(input[i]);
		size_t extra = 0;
		bool valid = false;

		if ((c & 0x80) == 0x00) {
			extra = 0;
			valid = true;
		} else if ((c & 0xE0) == 0xC0) {
			extra = 1;
			valid = (c >= 0xC2);
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			valid = true;
		} else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			valid = (c <= 0xF4);
		}

		if (valid) {
			if (i + extra >= input.size()) {
				valid = false;
			} else {
				for (size_t j = 1; j <= extra; ++j) {
					if (!isUtf8ContinuationByte(static_cast<unsigned char>(input[i + j]))) {
						valid = false;
						break;
					}
				}
			}
		}

		if (valid) {
			out.append(input, i, extra + 1);
			i += extra + 1;
		} else {
			out.push_back('?');
			++i;
		}
	}

	return out;
}

void sanitizeJsonStringsRecursive(json& value) {
	if (value.is_string()) {
		value = sanitizeUtf8String(value.get<std::string>());
		return;
	}
	if (value.is_object()) {
		for (auto it = value.begin(); it != value.end(); ++it) {
			sanitizeJsonStringsRecursive(it.value());
		}
		return;
	}
	if (value.is_array()) {
		for (auto& item : value) {
			sanitizeJsonStringsRecursive(item);
		}
	}
}

bool isAtOnlyMessage(const std::string& raw_message) {
	std::string trimmed = sanitizeUtf8String(raw_message);
	auto start = trimmed.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) return false;
	trimmed = trimmed.substr(start);
	auto end = trimmed.find_last_not_of(" \t\r\n");
	if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
	return trimmed.rfind("[CQ:at,qq=", 0) == 0 && trimmed.find(']') == trimmed.size() - 1;
}

bool tryParseUserId(const json& args, int64_t& uid) {
	if (!args.is_object() || !args.contains("user_id")) return false;
	const auto& value = args.at("user_id");
	try {
		if (value.is_number_integer()) {
			uid = value.get<int64_t>();
		}
		else if (value.is_number_unsigned()) {
			auto parsed = value.get<uint64_t>();
			if (parsed > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return false;
			uid = static_cast<int64_t>(parsed);
		}
		else if (value.is_string()) {
			std::string text = value.get<std::string>();
			size_t parsedChars = 0;
			uid = std::stoll(text, &parsedChars);
			if (parsedChars != text.size()) return false;
		}
		else {
			return false;
		}
	}
	catch (...) {
		return false;
	}
	return uid > 0;
}

json parseFirstJsonObject(const std::string& text) {
	size_t start = text.find('{');
	if (start == std::string::npos) return json::object();

	int depth = 0;
	bool inString = false;
	bool escaped = false;
	for (size_t i = start; i < text.size(); ++i) {
		char c = text[i];
		if (inString) {
			if (escaped) {
				escaped = false;
			}
			else if (c == '\\') {
				escaped = true;
			}
			else if (c == '"') {
				inString = false;
			}
			continue;
		}

		if (c == '"') {
			inString = true;
		}
		else if (c == '{') {
			depth++;
		}
		else if (c == '}') {
			depth--;
			if (depth == 0) {
				try {
					return json::parse(text.substr(start, i - start + 1));
				}
				catch (...) {
					return json::object();
				}
			}
		}
	}
	return json::object();
}

std::string jsonStringField(const json& obj, const std::string& key, const std::string& fallback = "") {
	if (!obj.is_object() || !obj.contains(key)) return fallback;
	const auto& value = obj.at(key);
	if (value.is_string()) return value.get<std::string>();
	return fallback;
}

bool jsonBoolField(const json& obj, const std::string& key, bool fallback = false) {
	if (!obj.is_object() || !obj.contains(key)) return fallback;
	const auto& value = obj.at(key);
	if (value.is_boolean()) return value.get<bool>();
	if (value.is_string()) {
		std::string v = value.get<std::string>();
		std::transform(v.begin(), v.end(), v.begin(), ::tolower);
		return v == "true" || v == "1" || v == "yes";
	}
	return fallback;
}

bool passesStableChance(const std::string& seed, double chance) {
	if (chance <= 0.0) return false;
	if (chance >= 1.0) return true;
	size_t hash = std::hash<std::string>{}(seed);
	double value = static_cast<double>(hash % 10000) / 10000.0;
	return value < chance;
}
}
// 配置从 Config 单例加载
Napcat::Napcat()
{
	const auto& nc = Config::get().napcat();
	NAPCAT_WS_URL   = nc.ws_url;
	NAPCAT_HTTP_URL = nc.http_url;
	_token          = nc.token;
}

// 发送间隔控制
void Napcat::throttleSend() {
	auto now = std::chrono::steady_clock::now();
	auto elapsed = now - _lastSendTime;
	if (elapsed < SEND_INTERVAL) {
		std::this_thread::sleep_for(SEND_INTERVAL - elapsed);
	}
	_lastSendTime = std::chrono::steady_clock::now();
}

// 获取对应 agent（群聊共享，私聊每人独立上下文）
agent& Napcat::getAgent(int64_t user_id, bool isGroup) {
	if (isGroup) return _agentGroup;
	auto it = _privateAgents.find(user_id);
	if (it == _privateAgents.end()) {
		auto ag = std::make_unique<agent>();
		ag->init();
		ag->setRestrictedMode(true);
		ag->setAllowedExecUsers(Config::get().napcat().admin_users);
		ag->setCurrentUser(std::to_string(user_id));
		registerBotTools(ag->tools);
		std::cout << "[Agent] 为私聊用户 " << user_id << " 创建独立会话" << std::endl;
		it = _privateAgents.emplace(user_id, std::move(ag)).first;
		// selfId 就绪时拉取私聊历史
		if (!_selfId.empty()) loadPrivateHistory(user_id);
	}
	return *it->second;
}

// 解析 OneBot 历史消息并注入 agent
static std::string formatHistoryMessages(const json& data, const std::string& selfId) {
	if (!data.contains("data") || !data["data"].contains("messages")) return "";
	auto& msgs = data["data"]["messages"];
	if (!msgs.is_array() || msgs.empty()) return "";

	std::ostringstream oss;
	int count = 0;
	for (auto it = msgs.rbegin(); it != msgs.rend() && count < 30; ++it) {
		auto& m = *it;
		int64_t uid = 0;
		if (m.contains("sender")) {
			auto& s = m["sender"];
			uid = s.value("user_id", int64_t(0));
			std::string card = s.value("card", "");
			std::string nick = s.value("nickname", "");
			std::string name = !card.empty() ? card : (!nick.empty() ? nick : std::to_string(uid));
			std::string raw = m.value("raw_message", "");
			if (raw.empty()) continue;
			bool isBot = (std::to_string(uid) == selfId);
			if (isBot)
				oss << "[bot → ]: " << raw << "\n";
			else
				oss << "[" << name << "(" << uid << ")]: " << raw << "\n";
			count++;
		} else {
			// 无 sender 的消息（系统通知等），只要有 raw_message 就保留
			std::string raw = m.value("raw_message", "");
			if (raw.empty()) continue;
			oss << "[系统]: " << raw << "\n";
			count++;
		}
	}
	std::cout << "[历史格式化] 输入 " << msgs.size() << " 条，输出 " << count << " 条" << std::endl;
	if (count == 0) return "";
	return oss.str();
}

void Napcat::loadGroupHistory() {
	if (_groupHistoryLoaded) return;

	const auto& whitelist = Config::get().napcat().group_whitelist;
	if (whitelist.empty()) return;
	_groupHistoryLoaded = true;

	for (int64_t gid : whitelist) {
		// 1) 先从本地 SQLite 同步拿最近消息兜底（NapCat API 可能还没准备好）
		std::string dbHistory = Memory::get().recent(gid, "group", 30);
		if (!dbHistory.empty()) {
			_agentGroup.injectHistoryCompressed("【群聊 " + std::to_string(gid) + " 历史记录(本地)】\n" + dbHistory);
			std::cout << "[历史] 已从本地数据库恢复群 " << gid << " 上下文 ("
				<< std::count(dbHistory.begin(), dbHistory.end(), '\n') << " 条消息)" << std::endl;
		}

		// 2) 异步从 NapCat 补齐（可能有本地缺失的消息）
		getGroupMsgHistory(gid, [this, gid](const json& resp) {
			std::string history = formatHistoryMessages(resp, _selfId);
			if (!history.empty()) {
				_agentGroup.injectHistoryCompressed("【群聊 " + std::to_string(gid) + " 最近对话(NapCat)】\n" + history);
				std::cout << "[历史] 已从 NapCat 恢复群 " << gid << " 上下文 ("
					<< std::count(history.begin(), history.end(), '\n') << " 条消息)" << std::endl;
			}
		});
	}
}

void Napcat::loadPrivateHistory(int64_t user_id) {
	if (_privateHistoryLoaded.count(user_id)) return;
	_privateHistoryLoaded.insert(user_id);

	auto it = _privateAgents.find(user_id);
	if (it == _privateAgents.end()) return;

	// 1) 先从本地 SQLite 同步拿
	std::string dbHistory = Memory::get().recent(user_id, "private", 30);
	if (!dbHistory.empty()) {
		it->second->injectHistoryCompressed("【私聊 " + std::to_string(user_id) + " 历史记录(本地)】\n" + dbHistory);
		std::cout << "[历史] 已从本地数据库恢复私聊 " << user_id << " ("
			<< std::count(dbHistory.begin(), dbHistory.end(), '\n') << " 条消息)" << std::endl;
	}

	// 2) 异步从 NapCat 补齐
	getFriendMsgHistory(user_id, [this, user_id](const json& resp) {
		auto it2 = _privateAgents.find(user_id);
		if (it2 == _privateAgents.end()) return;
		std::string history = formatHistoryMessages(resp, _selfId);
		if (!history.empty()) {
			it2->second->injectHistoryCompressed("【私聊 " + std::to_string(user_id) + " 最近对话(NapCat)】\n" + history);
			std::cout << "[历史] 已从 NapCat 恢复私聊 " << user_id << " ("
				<< std::count(history.begin(), history.end(), '\n') << " 条消息)" << std::endl;
		}
	});
}

// 底层：带 echo 的 API 调用
void Napcat::callApi(const std::string& action, const json& params, ApiCallback callback) {
	std::string echo = std::to_string(_echoCounter.fetch_add(1));

	{
		std::lock_guard<std::mutex> lock(_callbackMutex);
		_pendingCallbacks[echo] = std::move(callback);
	}

	json api_call = {
		{"action", action},
		{"params", params},
		{"echo", echo}
	};

	std::cout << "[API] → " << action << " (echo=" << echo << ")" << std::endl;
	_ws.send(api_call.dump());
}

// 底层：不带 echo 的 fire-and-forget
void Napcat::sendAction(const std::string& action, const json& params) {
	try {
		json safeParams = params;
		if (safeParams.is_object()) {
			for (auto it = safeParams.begin(); it != safeParams.end(); ++it) {
				if (it.value().is_string()) {
					it.value() = sanitizeUtf8(it.value().get<std::string>());
				}
			}
		}
		json api_call = {
			{"action", action},
			{"params", safeParams}
		};
		_ws.send(api_call.dump());
	} catch (const std::exception& e) {
		std::cerr << "[API发送] 异常: " << e.what() << std::endl;
	}
}

// 匹配 API 响应
bool Napcat::tryHandleApiResponse(const json& data) {
	if (!data.contains("echo") || !data["echo"].is_string()) {
		return false;
	}

	std::string echo = data["echo"].get<std::string>();

	ApiCallback cb;
	{
		std::lock_guard<std::mutex> lock(_callbackMutex);
		auto it = _pendingCallbacks.find(echo);
		if (it != _pendingCallbacks.end()) {
			cb = std::move(it->second);
			_pendingCallbacks.erase(it);
		}
	}

	if (cb) {
		std::cout << "[API] ← 响应 (echo=" << echo << ")" << std::endl;
		cb(data);
		return true;
	}
	return false;
}

// 便捷 API
std::string Napcat::getLoginInfo() {
	static std::string lastNickname;
	callApi("get_login_info", json::object(), [](const json& res) {
		if (res["status"] == "ok" && res.contains("data") && res["data"].contains("nickname")) {
			lastNickname = res["data"]["nickname"].get<std::string>();
		}
	});
	return lastNickname;
}

void Napcat::getGroupList(ApiCallback callback) {
	callApi("get_group_list", json::object(), std::move(callback));
}

void Napcat::getGroupMemberList(int64_t group_id, ApiCallback callback) {
	callApi("get_group_member_list", {{"group_id", group_id}}, std::move(callback));
}

void Napcat::getGroupInfo(int64_t group_id, ApiCallback callback) {
	callApi("get_group_info", {{"group_id", group_id}}, std::move(callback));
}

void Napcat::getFriendList(ApiCallback callback) {
	callApi("get_friend_list", json::object(), std::move(callback));
}

void Napcat::getStatus(ApiCallback callback) {
	callApi("get_status", json::object(), std::move(callback));
}

void Napcat::getGroupMsgHistory(int64_t group_id, ApiCallback callback) {
	callApi("get_group_msg_history", { {"group_id", group_id},{"count", 30} }, std::move(callback));
}

void Napcat::getFriendMsgHistory(int64_t user_id, ApiCallback callback) {
	callApi("get_friend_msg_history", { {"user_id", user_id},{"count", 30} }, std::move(callback));
}

void Napcat::sendLike(int64_t user_id) {
	std::cout << "[点赞] 用户 " << user_id << std::endl;
	sendAction("send_like", {
		{"user_id", user_id},
		{"times", 10}
		});
}

void Napcat::sendGroupMsg(int64_t group_id, const std::string& message) {
	throttleSend();
	std::cout << "[发送] 群 " << group_id << " : " << message << std::endl;
	sendAction("send_group_msg", {
		{"group_id", group_id},
		{"message", message}
	});
	int64_t botId = 0;
	if (!_selfId.empty()) botId = std::stoll(_selfId);
	Memory::get().store(group_id, "group", botId, "bot", message, true);
	StyleLearner::get().recordMessageStyle(group_id, message, "bot");
}

void Napcat::sendPrivateMsg(int64_t user_id, const std::string& message) {
	throttleSend();
	std::cout << "[发送] 私聊 " << user_id << " : " << message << std::endl;
	sendAction("send_private_msg", {
		{"user_id", user_id},
		{"message", message}
	});
	int64_t botId = 0;
	if (!_selfId.empty()) botId = std::stoll(_selfId);
	Memory::get().store(user_id, "private", botId, "bot", message, true);
}

std::string Napcat::sanitizeUtf8(const std::string& input) {
	return sanitizeUtf8String(input);
}

std::string Napcat::utf8SafeTruncate(const std::string& input, size_t maxBytes) {
	if (input.size() <= maxBytes) return input;
	size_t end = maxBytes;
	while (end > 0 && isUtf8ContinuationByte(static_cast<unsigned char>(input[end - 1]))) {
		--end;
	}
	return input.substr(0, end);
}

void Napcat::handleQuickOperation(const json& context, const json& operation) {
	try {
		if (!context.is_object() || !operation.is_object()) {
			std::cerr << "[快速操作] context/operation 必须是 object" << std::endl;
			return;
		}
		throttleSend();
		json safeContext = context;
		json safeOperation = operation;
		sanitizeJsonStringsRecursive(safeContext);
		sanitizeJsonStringsRecursive(safeOperation);

		json params = {
			{"context", safeContext},
			{"operation", safeOperation}
		};
		std::cout << "[快速操作] " << utf8SafeTruncate(safeOperation.dump(), 500) << std::endl;
		sendAction(".handle_quick_operation", params);
	} catch (const std::exception& e) {
		std::cerr << "[快速操作] 异常: " << e.what() << std::endl;
	}
}

void Napcat::quickReplyLong(const json& context, const std::string& text,
	bool atSender, bool autoEscape) {
	const size_t MAX_LEN = 2000;
	std::string safeText = sanitizeUtf8(text);

	// 判断是否为私聊
	std::string msgType = "group";
	int64_t userId = 0;
	if (context.contains("message_type") && context["message_type"] == "private") {
		msgType = "private";
		if (context.contains("user_id")) userId = context["user_id"].get<int64_t>();
	}

	if (safeText.size() <= MAX_LEN) {
		if (msgType == "private" && userId > 0) {
			sendPrivateMsg(userId, safeText);
		} else {
			quickReply(context, safeText, atSender, autoEscape);
		}
		return;
	}

	size_t pos = 0;
	int part = 1;
	while (pos < safeText.size()) {
		size_t end = std::min(pos + MAX_LEN, safeText.size());
		if (end < safeText.size()) {
			end = utf8SafeTruncate(safeText.substr(pos, end - pos), end - pos).size() + pos;
			if (end <= pos) end = std::min(pos + MAX_LEN, safeText.size());
		}
		std::string chunk = safeText.substr(pos, end - pos);
		std::string label = "[" + std::to_string(part) + "] " + chunk;
		part++;

		if (msgType == "private" && userId > 0) {
			sendPrivateMsg(userId, label);
		} else {
			quickReply(context, label, atSender, autoEscape);
		}

		pos = end;
		while (pos < safeText.size() && (safeText[pos] == '\n' || safeText[pos] == ' ')) pos++;
	}
}

void Napcat::quickReply(const json& context, const std::string& text,
	bool atSender, bool autoEscape) {
	if (context.contains("message_type") && context["message_type"] == "private") {
		if (context.contains("user_id")) {
			sendPrivateMsg(context["user_id"].get<int64_t>(), sanitizeUtf8(text));
		}
		return;
	}

	json safeContext = context;
	sanitizeJsonStringsRecursive(safeContext);
	handleQuickOperation(safeContext, {
		{"reply", sanitizeUtf8(text)},
		{"at_sender", atSender},
		{"auto_escape", autoEscape}
	});
}
void Napcat::quickKick(const json& context) {
	handleQuickOperation(context, {{"kick", true}});
}
void Napcat::quickBan(const json& context, int64_t durationSeconds) {
	handleQuickOperation(context, {{"ban", true}, {"ban_duration", durationSeconds}});
}
void Napcat::quickApprove(const json& context, const std::string& remark) {
	json op = {{"approve", true}};
	if (!remark.empty()) op["remark"] = remark;
	handleQuickOperation(context, op);
}
void Napcat::quickReject(const json& context, const std::string& reason) {
	json op = {{"approve", false}};
	if (!reason.empty()) op["reason"] = reason;
	handleQuickOperation(context, op);
}
void Napcat::quickSetRemark(const json& context, const std::string& remark) {
	handleQuickOperation(context, {{"remark", remark}});
}

// 检测消息是否 @了机器人（通过 CQ 码匹配）
bool Napcat::isAtBot(const std::string& raw_message) const {
	if (_selfId.empty()) return false;
	// OneBot v11 @ 格式: [CQ:at,qq=<qq号>]
	std::string cq_at = "[CQ:at,qq=" + _selfId;
	return raw_message.find(cq_at) != std::string::npos;
}

// 检测是否为骚扰式重复 @
bool Napcat::isAtSpamming(int64_t user_id, const std::string& message_content) {
	// 计算消息哈希（简单方案：前50字符）
	std::string hash = message_content.substr(0, std::min((size_t)50, message_content.size()));
	auto now = std::chrono::steady_clock::now();

	// 清理过期记录（超过时间窗口的）
	auto it = _atMessageHistory.begin();
	while (it != _atMessageHistory.end()) {
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->timestamp).count();
		if (elapsed > AT_SPAM_WINDOW_MS) {
			it = _atMessageHistory.erase(it);
		} else {
			++it;
		}
	}

	// 统计该用户在时间窗口内相同内容的 @ 次数
	int count = 0;
	for (const auto& record : _atMessageHistory) {
		if (record.user_id == user_id && record.message_hash == hash) {
			count++;
		}
	}

	// 添加当前消息记录
	_atMessageHistory.push_back({user_id, hash, now});
	if (_atMessageHistory.size() > AT_MESSAGE_HISTORY_MAX) {
		_atMessageHistory.erase(_atMessageHistory.begin());
	}

	// 如果达到骚扰阈值，返回 true（是骚扰）
	return count >= AT_SPAM_THRESHOLD;
}

// 检查并触发空闲补偿（群内长时间无消息时主动开启话题）
void Napcat::checkAndTriggerIdleCompensation(int64_t group_id) {
	auto now = std::chrono::steady_clock::now();
	auto it = _groupIdleStates.find(group_id);

	// 首次见到这个群
	if (it == _groupIdleStates.end()) {
		_groupIdleStates[group_id] = {group_id, now, false};
		return;
	}

	GroupIdleState& idleState = it->second;
	auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - idleState.lastActivityTime).count();

	// 检查是否触发空闲补偿条件
	if (elapsed >= IDLE_THRESHOLD_SECONDS && !idleState.hasTriggeredIdle) {
		idleState.hasTriggeredIdle = true;
		std::cout << "[空闲补偿] 群 " << group_id << " 已空闲 " << elapsed
			<< " 秒，触发主动开启话题" << std::endl;

		// 获取群聊 agent
		if (!_groupAgentReady) return;

		agent& currentAgent = getAgent(0, true);
		currentAgent.setCurrentUser("bot_idle_trigger");
		currentAgent.setChatId(group_id, "group");

		// 生成补偿消息（让 LLM 主动开启话题）
		std::string idlePrompt =
			"群聊已经很久没有新消息了。请生成一条自然、轻松的开场白来重新活跃气氛。"
			"不要问问题，而是分享一个有趣的观点或故事。保持简短（1-2句话）。";

		json messages = json::array();
		messages.push_back({{"role", "user"}, {"content", idlePrompt}});

		try {
			json result = currentAgent.models.deepseek.SendChatCompletion(
				currentAgent.getCurlHandle(), messages, json::array());

			std::string idleMessage;
			if (result.contains("choices") && !result["choices"].empty()
				&& result["choices"][0].contains("message")
				&& result["choices"][0]["message"].contains("content")) {
				idleMessage = result["choices"][0]["message"]["content"].get<std::string>();

				// 发送补偿消息到群
				sendGroupMsg(group_id, idleMessage);
				std::cout << "[空闲补偿] 已发送: " << idleMessage << std::endl;
			}
		}
		catch (const std::exception& e) {
			std::cerr << "[空闲补偿] 生成消息失败: " << e.what() << std::endl;
		}
	}

	// 更新最后活动时间
	idleState.lastActivityTime = now;
}

// 事件处理（消息管线模式）
void Napcat::handleMessage(const json& data) {
	// 优先作为 API 响应处理
	if (tryHandleApiResponse(data)) {
		return;
	}

	// 触发 ON_MESSAGE 事件
	{
		Event event;
		event.type = EventType::ON_MESSAGE;
		event.tag = "message_received";
		event.timestamp = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000;
		if (data.contains("raw_message")) {
			event.data["message"] = sanitizeUtf8(data["raw_message"].get<std::string>());
		}
		if (data.contains("message_type")) {
			event.data["type"] = sanitizeUtf8(data["message_type"].get<std::string>());
		}
		if (data.contains("sender") && data["sender"].contains("user_id")) {
			event.data["user_id"] = std::to_string(data["sender"]["user_id"].get<int64_t>());
		}
		EventBus::get().fireIntercept(event);  // 同步调用，允许中断
	}

	// 提取 self_id（首次收到时缓存，同时触发历史恢复）
	if (_selfId.empty() && data.contains("self_id")) {
		if (data["self_id"].is_number()) {
			_selfId = std::to_string(data["self_id"].get<int64_t>());
		} else if (data["self_id"].is_string()) {
			_selfId = data["self_id"].get<std::string>();
		}
		std::cout << "[信息] Bot QQ: " << _selfId << std::endl;
		loadGroupHistory();
	}

	// 安全提取字段
	auto safeGetInt64 = [](const json& d, const std::string& key) -> int64_t {
		if (!d.contains(key)) return 0;
		if (d[key].is_number()) return d[key].get<int64_t>();
		if (d[key].is_string()) {
			try { return std::stoll(d[key].get<std::string>()); }
			catch (...) { return 0; }
		}
		return 0;
	};

	std::string post_type = data.value("post_type", "");
	std::string message_type = data.value("message_type", "");
	std::string raw_message = data.value("raw_message", "");
	int64_t group_id = safeGetInt64(data, "group_id");
	json sender = data.value("sender", json::object());
	int64_t user_id = safeGetInt64(sender, "user_id");
	std::string card = sender.value("card", "");
	std::string nickname = sender.value("nickname", "");
	std::string displayName = !card.empty() ? card : (!nickname.empty() ? nickname : std::to_string(user_id));

	// Stage 0: 过滤非消息事件
	if (post_type != "message") {
		return;
	}

	// Stage 0.5: 群白名单
	if (message_type == "group") {
		const auto& wl = Config::get().napcat().group_whitelist;
		if (!wl.empty() && !wl.count(group_id)) return;
	}

	// Stage 1: 持久化（无论是否回复都存）
	bool isGroup = (message_type == "group");
	Memory::get().store(isGroup ? group_id : user_id,
		isGroup ? "group" : "private",
		user_id, displayName, raw_message, false);

	std::cout << "[收到] 类型: " << message_type
		<< " | 内容: " << raw_message << std::endl;

	// 更新群聊空闲状态
	if (isGroup) {
		checkAndTriggerIdleCompensation(group_id);

		// 行话挖掘：扫描新词汇
		std::vector<std::string> recentContext;
		auto recent = Memory::get().recentTextContext(group_id, "group", 3, user_id, raw_message);
		std::istringstream iss(recent);
		std::string line;
		while (std::getline(iss, line) && recentContext.size() < 3) {
			if (!line.empty()) recentContext.push_back(line);
		}
		JargonMiner::get().scanMessage(group_id, std::to_string(user_id), raw_message, recentContext);

		// 记录群友消息风格（用于群风格分析回写）
		StyleLearner::get().recordMessageStyle(group_id, raw_message, std::to_string(user_id));

		// ── 功能 2：行话库学习 ──
		KnowledgeRetriever::get().learn_jargon(group_id, std::to_string(user_id), raw_message, recentContext);

		// ── 功能 3：相似消息去重 ──
		if (KnowledgeRetriever::get().is_duplicate_message(group_id, raw_message, 0.85f)) {
			std::cout << "[去重] 检测到重复消息，跳过处理" << std::endl;
			return;
		}
	}

	std::string turnSender = displayName + "(" + std::to_string(user_id) + ")";
	if (isGroup && _groupAgentReady && _agentGroup.isReasoning()) {
		_agentGroup.injectTurnMessage(raw_message, turnSender);
		std::cout << "[Reasoning] 群聊新消息已注入当前推理" << std::endl;
		return;
	}
	if (!isGroup) {
		auto it = _privateAgents.find(user_id);
		if (it != _privateAgents.end() && it->second && it->second->isReasoning()) {
			it->second->injectTurnMessage(raw_message, turnSender);
			std::cout << "[Reasoning] 私聊新消息已注入当前推理" << std::endl;
			return;
		}
	}

	// Stage 2: 屏蔽词过滤 
	if (checkBanWords(raw_message)) {
		std::cout << "[过滤] 命中屏蔽词，丢弃消息" << std::endl;
		return;
	}

	// Stage 3: 命令匹配
	if (checkCommand(data, raw_message, data)) {
		return;  // 命令已处理完毕，不继续走 AI
	}

	// Stage 3.5: 私聊白名单 
	if (message_type == "private") {
		const auto& pwl = Config::get().napcat().private_whitelist;
		if (!pwl.empty() && !pwl.count(user_id)) return;
	}

	// Stage 5: 是否需要 AI 评估
	bool isAtBotMsg = isAtBot(raw_message);
	bool hasImage = raw_message.find("[CQ:image") != std::string::npos;

	// 群聊中：只保留原始消息事实，不在入口做 OCR 状态切换；由 agent tool-call 决定是否识别图片
	if (message_type == "group") {
		(void)hasImage;
	}

	std::string activeSessionKey = isGroup ? "group:" + std::to_string(group_id) : "private:" + std::to_string(user_id);
	bool isForcedTrigger = isAtBotMsg;
	bool idleInitiated = false;
	const auto& pipeline = Config::get().pipeline();
	if (!isForcedTrigger && isGroup && shouldUseIdleCompensation(activeSessionKey, pipeline.idleCompensationSeconds)) {
		idleInitiated = true;
		std::cout << "[空闲补偿] 群聊空闲超时，主动开场" << std::endl;
	}

	// 空闲补偿：主动开场
	if (idleInitiated) {
		// 空闲补偿也需要检查冷却时间
		{
			std::lock_guard<std::mutex> lock(_replyTimeMutex);
			auto lastReplyIt = _lastAiReplyTimes.find(activeSessionKey);
			if (lastReplyIt != _lastAiReplyTimes.end() && pipeline.cooldownSeconds > 0) {
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - lastReplyIt->second).count();
				if (elapsed < pipeline.cooldownSeconds) {
					std::cout << "[评估] 空闲补偿冷却中，剩余 " << (pipeline.cooldownSeconds - elapsed)
						<< " 秒" << std::endl;
					return;
				}
			}
		}
		if (!_groupAgentReady) return;
		agent& currentAgent = getAgent(user_id, isGroup);
		currentAgent.setCurrentUser(std::to_string(user_id));
		_currentGroupId = group_id;
		_currentUserId = user_id;
		_currentMsgType = message_type;
		currentAgent.setChatId(group_id, message_type);
		currentAgent.setSpeaker(user_id, displayName);

		struct CurrentMessageContextGuard {
			~CurrentMessageContextGuard() { g_currentMessageContext = json(); }
		} contextGuard;
		g_currentMessageContext = data;

		std::string recentContext = Memory::get().recentTextContext(group_id, "group", 8, user_id, raw_message);
		std::string initiatePrompt = "群聊已经空闲一段时间了。请根据最近的聊天内容，主动发起一个自然的话题或回应，让气氛重新活跃起来。\n\n";
		if (!recentContext.empty()) {
			initiatePrompt += "=== 最近群聊上下文 ===\n" + recentContext + "\n";
		}
		initiatePrompt += "请用自然、轻松的语气开场，不要显得突兀。可以：\n"
			"- 延续之前未完的话题\n"
			"- 分享有趣的观点或问题\n"
			"- 简单打个招呼或调侃\n";

		std::string aiReply = currentAgent.query(initiatePrompt);
		if (!aiReply.empty()) {
			std::string cleanAiReply = sanitizeUtf8(aiReply);
			quickReplyLong(data, cleanAiReply);
			LongMemoryContext memoryCtx;
			memoryCtx.chat_id = group_id;
			memoryCtx.chat_type = "group";
			memoryCtx.user_id = user_id;
			memoryCtx.user_name = sanitizeUtf8(displayName);
			memoryCtx.user_message = sanitizeUtf8("[空闲补偿触发]");
			memoryCtx.bot_reply = cleanAiReply;
			LongMemory::get().writeBackAfterReply(memoryCtx);
			std::string userCooldownKey = "user_" + std::to_string(user_id);
			recordAiReplyTime(userCooldownKey);
		}
		return;
	}

	// 是否注入最近群聊上下文（@ 消息和未@但评估通过的都注入）
	bool shouldInjectGroupContext = false;
	if (message_type == "group") {
		const auto& pipeline = Config::get().pipeline();
		if (!isForcedTrigger) {
			if (!passesStableChance(activeSessionKey + ":" + raw_message, pipeline.unmentionedEvalChance)) {
				std::cout << "[评估] unmentionedEvalChance=" << pipeline.unmentionedEvalChance
					<< "，本条未进入评估" << std::endl;
				return;
			}
		}
		{
			std::lock_guard<std::mutex> lock(_replyTimeMutex);
			auto lastReplyIt = _lastAiReplyTimes.find(activeSessionKey);
			if (lastReplyIt != _lastAiReplyTimes.end() && pipeline.cooldownSeconds > 0) {
				auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::steady_clock::now() - lastReplyIt->second).count();
				if (elapsed < pipeline.cooldownSeconds) {
					std::cout << "[评估] 冷却中，剩余 " << (pipeline.cooldownSeconds - elapsed)
						<< " 秒" << std::endl;
					return;
				}
			}
		}
		if (!_groupAgentReady) {
			return;  // AI 未就绪，静默忽略
		}
		// 提前拉取最近群聊上下文，供评估时使用
		std::string recentContext = Memory::get().recentTextContext(group_id, "group", 8, user_id, raw_message);

		agent& currentAgent = getAgent(user_id, isGroup);
		currentAgent.setCurrentUser(std::to_string(user_id));
		_currentGroupId = group_id;
		_currentUserId = user_id;
		_currentMsgType = message_type;

		struct CurrentMessageContextGuard {
			~CurrentMessageContextGuard() { g_currentMessageContext = json(); }
		} contextGuard;
		g_currentMessageContext = data;

		// @ 消息的优先级最高：跳过 Timing Gate、ReplyDecision 和冷却时间，直接进入回复流程
		// 但检查是否为骚扰式重复 @
		bool isAtMessage = isAtBot(raw_message);

		if (isAtMessage) {
			if (isAtOnlyMessage(raw_message) && raw_message.find("[CQ:image") == std::string::npos) {
				std::cout << "[Timing Gate] 空 @ 消息，直接忽略" << std::endl;
				return;
			}
			if (isAtSpamming(user_id, raw_message)) {
				std::cout << "[Timing Gate] 检测到骚扰式重复 @，拒绝回复" << std::endl;
				return;
			}
			std::cout << "[Timing Gate] @ 消息检测到，优先级最高，直接回复（无冷却限制）" << std::endl;
			// @ 消息直接跳过 Timing Gate、ReplyDecision 和冷却时间，100% 回复
			shouldInjectGroupContext = true;
		} else {
			// 非 @ 消息需要检查冷却时间（改为用户级冷却）
			{
				std::lock_guard<std::mutex> lock(_replyTimeMutex);
				std::string userCooldownKey = "user_" + std::to_string(user_id);
				auto lastReplyIt = _lastAiReplyTimes.find(userCooldownKey);
				if (lastReplyIt != _lastAiReplyTimes.end() && pipeline.cooldownSeconds > 0) {
					auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
						std::chrono::steady_clock::now() - lastReplyIt->second).count();
					if (elapsed < pipeline.cooldownSeconds) {
						std::cout << "[评估] 用户 " << user_id << " 冷却中，剩余 " << (pipeline.cooldownSeconds - elapsed)
							<< " 秒" << std::endl;
						return;
					}
				}
			}
			// 非 @ 消息才需要 Timing Gate 评估和 ReplyDecision 判断
			TimingGateDecision gate = evaluateTimingGate(raw_message, group_id, recentContext, currentAgent);
			if (gate == TimingGateDecision::NoAction) return;
			if (gate == TimingGateDecision::Wait) {
				std::cout << "[Timing Gate] 决定等待，暂不回复" << std::endl;
				return;
			}

			ReplyDecision decision = evaluateShouldReply(raw_message, group_id, recentContext, currentAgent);
			if (decision == ReplyDecision::Silence) return;
			if (decision == ReplyDecision::Cooldown) {
				std::cout << "[评估] 冷却中，暂不插话" << std::endl;
				return;
			}
			// ReplyDecision::Reply → 走下面的回复流程
			shouldInjectGroupContext = true;
		}
	}

	if (!_groupAgentReady) {
		quickReplyLong(data, sanitizeUtf8("AI 引擎未就绪，请稍后再试"));

		return;
	}

	// 记录会话上下文
	_currentGroupId = group_id;
	_currentUserId = user_id;
	_currentMsgType = message_type;

	struct CurrentMessageContextGuard {
		~CurrentMessageContextGuard() { g_currentMessageContext = json(); }
	} contextGuard;
	g_currentMessageContext = data;

	// 路由到对应 agent
	agent& currentAgent = getAgent(user_id, isGroup);
	currentAgent.setCurrentUser(std::to_string(user_id));
	currentAgent.setChatId(isGroup ? group_id : user_id, message_type);
	currentAgent.setSpeaker(user_id, displayName);

	// 注入群风格到 agent
	if (isGroup) {
		std::string groupCtx = VocabularyManager::get().getGroupContext(group_id);
		if (!groupCtx.empty()) {
			currentAgent.injectGroupStyle(groupCtx);
		}
	}

	// 构造消息前缀
	std::string msgPrefix;
	if (isGroup) {
		msgPrefix = "[" + displayName + "(" + std::to_string(user_id) + ")]: ";
	} else {
		msgPrefix = "[私聊] ";
	}
	std::string ephemeralContext;
	if (shouldInjectGroupContext && isGroup) {
		ephemeralContext = Memory::get().recentTextContext(group_id, "group", 8, user_id, raw_message);
		if (!ephemeralContext.empty()) {
			std::cout << "[上下文] 注入最近群聊 "
				<< std::count(ephemeralContext.begin(), ephemeralContext.end(), '\n')
				<< " 条" << std::endl;
		}
	}
	std::string aiReply = ephemeralContext.empty()
		? currentAgent.query(msgPrefix + raw_message)
		: currentAgent.queryWithEphemeralContext(msgPrefix + raw_message, ephemeralContext);

	if (aiReply == "[__queued__]") {
		std::cout << "[Reasoning] 当前消息已并入进行中的推理" << std::endl;
		return;
	}

	if (!aiReply.empty()) {
		std::string cleanAiReply = sanitizeUtf8(aiReply);
		quickReplyLong(data, cleanAiReply);

		LongMemoryContext memoryCtx;
		memoryCtx.chat_id = isGroup ? group_id : user_id;
		memoryCtx.chat_type = sanitizeUtf8(isGroup ? "group" : "private");
		memoryCtx.user_id = user_id;
		memoryCtx.user_name = sanitizeUtf8(displayName);
		memoryCtx.user_message = sanitizeUtf8(raw_message);
		memoryCtx.bot_reply = cleanAiReply;
		LongMemory::get().writeBackAfterReply(memoryCtx);
		std::string userCooldownKey = "user_" + std::to_string(user_id);
		recordAiReplyTime(userCooldownKey);
	} else {
		std::cout << "[AI] 空回复，终止本次处理" << std::endl;
		return;
	}
}

// 向 Tools 注册 NapCat 机器人专属能力
void Napcat::registerBotTools(Tools& t) {
	// send_group_text — 向当前群聊发送文本
	t.registerTool("send_group_text", {
		{"type", "function"},
		{"function", {
			{"name", "send_group_text"},
			{"description", "向当前群聊主动发送文本消息（非回复某人）。仅群聊有效。"},
			{"parameters", {
				{"type", "object"},
				{"properties", {
					{"text", {
						{"type", "string"},
						{"description", "要发送的文本内容"}
					}}
				}},
				{"required", {"text"}}
			}}
		}}
		}, [this](const json& args) -> std::string {
			std::string text = args["text"];
			if (_currentGroupId == 0) return "当前不在群聊上下文中";
			sendGroupMsg(_currentGroupId, text);
			return "已发送到群 " + std::to_string(_currentGroupId);
		});

	// send_private_text — 向指定用户发送私聊文本
	t.registerTool("send_private_text", {
		{"type", "function"},
		{"function", {
			{"name", "send_private_text"},
			{"description", "向指定 QQ 用户发送私聊文本。user_id 从消息前缀[昵称(QQ号)]中获取。"},
			{"parameters", {
				{"type", "object"},
				{"properties", {
					{"user_id", {
						{"type", "string"},
						{"description", "目标用户的 QQ 号"}
					}},
					{"text", {
						{"type", "string"},
						{"description", "要发送的文本内容"}
					}}
				}},
				{"required", {"user_id", "text"}}
			}}
		}}
		}, [this](const json& args) -> std::string {
			std::string text = args["text"];
			int64_t uid = 0;
			if (!tryParseUserId(args, uid)) return "无效的用户 ID，请传入有效 QQ 号";
			sendPrivateMsg(uid, text);
			return "已发送私聊到 " + std::to_string(uid);
		});

	// send_like — 给用户点赞
	t.registerTool("send_like", {
		{"type", "function"},
		{"function", {
			{"name", "send_like"},
			{"description", "给指定 QQ 用户点赞 10 次。user_id 从消息前缀中获取。"},
			{"parameters", {
				{"type", "object"},
				{"properties", {
					{"user_id", {
						{"type", "string"},
						{"description", "目标用户的 QQ 号"}
					}}
				}},
				{"required", {"user_id"}}
			}}
		}}
		}, [this](const json& args) -> std::string {
			int64_t uid = 0;
			if (!tryParseUserId(args, uid)) return "无效的用户 ID，请传入有效 QQ 号";
			sendLike(uid);
			return "已给 " + std::to_string(uid) + " 点赞";
		});

	// get_group_msg_history — 获取群历史消息
	t.registerTool("get_group_msg_history", {
	  {"type", "function"},
	  {"function", {
		  {"name", "get_group_msg_history"},
			{"description", "从 NapCat 服务器拉取群聊最近 30 条消息并注入上下文。与 search_history（本地数据库）互补。仅群聊有效。"},
		  {"parameters", {
			  {"type", "object"},
			  {"properties", {
				  {"count", {
					  {"type", "integer"},
					  {"description", "获取消息条数，最大30"}
				  }}
			  }}
		  }}
	  }}
		}, [this](const json& args) -> std::string {
			if (_currentGroupId == 0) return "此操作仅群聊可用";
			int64_t gid = _currentGroupId;  // 拷贝值，回调里不用碰 this
			getGroupMsgHistory(gid, [this, gid](const json& resp) {
				std::string history = formatHistoryMessages(resp, _selfId);
				if (!history.empty()) {
					_agentGroup.injectHistoryCompressed(
						"【群聊 " + std::to_string(gid) + " 历史记录】\n" + history);
				}
				});
			return "正在从 NapCat 拉取群聊 " + std::to_string(gid) + " 的历史消息，稍后上下文会自动更新。";
		});

		// recognize_image — 由 agent 决定是否识别当前消息中的图片（仅 OCR 启用时注册）
		if (Config::get().ocr().enabled) {
			t.registerTool("recognize_image", {
				{"type", "function"},
				{"function", {
					{"name", "recognize_image"},
					{"description", "识别当前消息中的图片。仅当当前消息包含图片且需要 OCR 时调用。"},
					{"parameters", {
						{"type", "object"},
						{"properties", {
							{"max_images", { {"type", "integer"}, {"description", "最多识别的图片数量，默认2"} }}
						}},
						{"required", json::array()}
					}}
				}}
				}, [this](const json& args) -> std::string {
					int maxImages = args.value("max_images", 2);
					if (maxImages <= 0) maxImages = 2;
					if (maxImages > 5) maxImages = 5;
					std::string raw = g_currentMessageContext.value("raw_message", "");
					auto urls = ImageOcrService::get().extractImageUrls(raw, maxImages);
					if (urls.empty()) return "当前消息中未找到可识别的图片";
					return ImageOcrService::get().recognizeImages(urls);
				});
		}


	t.registerTool("search_history", {
		{"type", "function"},
		{"function", {
			{"name", "search_history"},
			{"description", "【本地数据库查询工具】查询当前会话存储在本地SQLite数据库中(消息.db)的历史消息。这是查询聊天记录的正确方法，不要用exec_cmd去操作.db文件。count=返回条数(≤200,默认50)，from/to=时间范围如\"06-07 18:00\"或\"2026-06-07 18:00\"，传keyword则全文搜索。条数较大时结果可能被AI自动摘要压缩。"},
			{"parameters", {
				{"type", "object"},
				{"properties", {
					{"count", {
						{"type", "integer"},
						{"description", "返回条数，默认50，最大200。条数较大时结果可能被AI自动摘要压缩"}
					}},
					{"from", {
						{"type", "string"},
						{"description", "起始时间，格式 \"2026-06-07 18:00\" 或 \"06-07 18:00\"，留空=不限"}
					}},
					{"to", {
						{"type", "string"},
						{"description", "结束时间，格式同上，留空=不限"}
					}},
					{"keyword", {
						{"type", "string"},
						{"description", "搜索关键词，留空则返回全部"}
					}}
				}}
			}}
		}}
		}, [this](const json& args) -> std::string {
			int count = args.value("count", 50); if (count > 200) count = 200; if (count <= 0) count = 50;
			auto parseTime = [](const std::string& s) -> int64_t {
				if (s.empty()) return 0;
				int yr = 0, mon = 0, day = 0, hr = 0, min = 0, sec = 0;
				int n = sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &yr, &mon, &day, &hr, &min, &sec);
				if (n >= 5) {
				}
				else {
					n = sscanf(s.c_str(), "%d-%d %d:%d:%d", &mon, &day, &hr, &min, &sec);
					if (n >= 4) {
						std::time_t now = std::time(nullptr);
						std::tm now_tm;
#ifdef _WIN32
						localtime_s(&now_tm, &now);
#else
						localtime_r(&now, &now_tm);
#endif
						yr = now_tm.tm_year + 1900;
					}
					else {
						return 0;
					}
				}
				std::tm tm = {};
				tm.tm_year = yr - 1900;
				tm.tm_mon = mon - 1;
				tm.tm_mday = day;
				tm.tm_hour = hr;
				tm.tm_min = min;
				tm.tm_sec = sec;
				tm.tm_isdst = -1;
				return (int64_t)mktime(&tm);
				};
			int64_t from_ts = parseTime(args.value("from", std::string()));
			int64_t to_ts = parseTime(args.value("to", std::string()));
			std::string keyword = args.value("keyword", "");
			bool isGroup = (_currentMsgType == "group");
			return isGroup
				? Memory::get().query(_currentGroupId, "group", count, from_ts, to_ts, keyword)
				: Memory::get().query(_currentUserId, "private", count, from_ts, to_ts, keyword);
		});

	// ban_user — 禁言当前消息发送者
	t.registerTool("ban_user", {
		{"type", "function"},
		{"function", {
			{"name", "ban_user"},
			{"description", "禁言群聊中的指定成员。默认禁言当前正在回复的消息发送者；也可显式传user_id。仅群聊可用。duration = 分钟，默认10，0 = 解除禁言。"},
			{"parameters", {
				{"type", "object"},
				{"properties", {
					{"user_id", {
						{"type", "string"},
						{"description", "目标用户 QQ 号。留空则默认当前正在回复的消息发送者"}
					}},
					{"duration", {
						{"type", "integer"},
						{"description", "禁言分钟数，默认10。0=解禁。最大43200(30天)"}
					}}
				}}
			}}
		}}
		}, [this](const json& args) -> std::string {
			if (_currentGroupId == 0) return "此操作仅群聊可用";

			int64_t minutes = args.value("duration", static_cast<int64_t>(10));
			if (minutes < 0) minutes = 0;
			if (minutes > 43200) minutes = 43200;

			int64_t targetUserId = _currentUserId;
			if (args.contains("user_id")) {
				const auto& juid = args["user_id"];
				if (juid.is_number_integer()) {
					targetUserId = juid.get<int64_t>();
				}
				else if (juid.is_string()) {
					try { targetUserId = std::stoll(juid.get<std::string>()); }
					catch (...) { return "无效的用户 ID"; }
				}
				else {
					return "无效的用户 ID";
				}
			}
			if (targetUserId == 0) return "无效的用户 ID";

			sendAction("set_group_ban", {
				{"group_id", std::to_string(_currentGroupId)},
				{"user_id", std::to_string(targetUserId)},
				{"duration", minutes * 60}
				});

			return minutes > 0 ? "已禁言用户 " + std::to_string(targetUserId) + " " + std::to_string(minutes) + " 分钟" : "已解除用户 " + std::to_string(targetUserId) + " 的禁言";
		});
}

// 消息管线方法

// 屏蔽词检查
bool Napcat::checkBanWords(const std::string& text) const {
    const auto& ban = Config::get().ban();
    for (auto& word : ban.ban_words) {
        if (text.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 命令匹配
bool Napcat::checkCommand(const json& data, const std::string& text,
    const json& context)
{
    std::string args;
    auto* cmd = _cmdHandler.match(text, args);
    if (!cmd) return false;

    if (cmd->level != CommandMatchLevel::Intercept) return false;

    // Special handling for "赞我" which needs sendLike side effect
    if (cmd->name == "赞我") {
        int64_t uid = 0;
        if (data.contains("sender")) {
            json sender2 = data["sender"];
            if (sender2.contains("user_id") && sender2["user_id"].is_number()) {
                uid = sender2["user_id"].get<int64_t>();
            }
        }
        if (uid > 0) sendLike(uid);
    }

    std::string result = _cmdHandler.execute(*cmd, args);
    quickReply(context, result);
    return true;
}

// ── Timing Gate：在未@消息达到频率阈值后，再判断是否真的该开口
Napcat::TimingGateDecision Napcat::evaluateTimingGate(
    const std::string& raw_message, int64_t group_id,
    const std::string& recent_context, agent& currentAgent)
{
    std::cout << "[Timing Gate] 进入时机判断" << std::endl;

    std::string evalPrompt =
        "你是群聊机器人发言时机判断器。请判断机器人现在是否该开口。\n\n";
    if (!recent_context.empty()) {
        evalPrompt += "=== 最近群聊上下文 ===\n" + recent_context + "\n";
    }
    evalPrompt +=
        "=== 当前新消息 ===\n" + raw_message + "\n\n"
        "只输出 JSON，不要输出其他内容：\n"
        "{\"action\":\"continue|no_action|wait\",\"reason\":\"一句话原因\"}\n\n"
        "规则：\n"
        "- continue：机器人现在说话会自然、有用或能接上话题。\n"
        "- no_action：当前消息和机器人无关，插话会突兀。\n"
        "- no_action：即使消息 @ 了机器人，但只是重复 @、纯 @、连续刷相同/相似内容、挑衅机器人反复回应，也不要开口。\n"
        "- wait：话题可能值得参与，但现在群友正在连续讨论，应先等等。\n";

    json messages_arr = json::array();
    messages_arr.push_back({ {"role", "user"}, {"content", evalPrompt} });
    json result = currentAgent.models.deepseek.SendChatCompletion(
        currentAgent.getCurlHandle(), messages_arr, json::array());

    std::string response;
    if (result.contains("choices") && !result["choices"].empty()
        && result["choices"][0].contains("message")
        && result["choices"][0]["message"].contains("content")) {
        response = result["choices"][0]["message"]["content"].get<std::string>();
    }

    json decision = parseFirstJsonObject(response);
    std::string action = jsonStringField(decision, "action", "no_action");
    if (action == "continue") {
        std::cout << "[Timing Gate] continue" << std::endl;
        return TimingGateDecision::Continue;
    }
    if (action == "wait") {
        std::cout << "[Timing Gate] wait" << std::endl;
        return TimingGateDecision::Wait;
    }

    std::cout << "[Timing Gate] no_action" << std::endl;
    return TimingGateDecision::NoAction;
}

// ── 群聊未@消息评估：规则前置 + LLM 上下文感知（JSON 输出）
Napcat::ReplyDecision Napcat::evaluateShouldReply(
    const std::string& raw_message, int64_t group_id,
    const std::string& recent_context, agent& currentAgent)
{
    const auto& pipeline = Config::get().pipeline();
    const auto& aliases = Config::get().napcat().bot_aliases;

    // ── 规则前置过滤：快速排除噪声 ──
    // 1) 过短
    size_t msg_len = raw_message.size();
    if (msg_len < 2) {
        std::cout << "[评估] 规则: 消息过短(" << msg_len << ")，跳过" << std::endl;
        return ReplyDecision::Silence;
    }

    // 2) 纯广告/引流：包含常见广告关键词 + 联系方式模式
    const auto& ad_kws = Config::get().ban().ad_keywords;
    if (!ad_kws.empty()) {
        for (const auto& kw : ad_kws) {
            if (raw_message.find(kw) != std::string::npos) {
                std::cout << "[评估] 规则: 疑似广告/引流，跳过" << std::endl;
                return ReplyDecision::Silence;
            }
        }
    }

    // 3) 纯表情包/emoji：没有汉字/字母/数字，主要是 emoji
    bool hasCjk = false;
    bool hasAlpha = false;
    for (char c : raw_message) {
        if ((c & 0x80) && !isalnum(c)) hasCjk = true;
        if (isalpha(c)) hasAlpha = true;
    }
    if (!hasCjk && !hasAlpha) {
        // 纯表情/emoji/数字，跳过
        std::cout << "[评估] 规则: 非文本消息，跳过" << std::endl;
        return ReplyDecision::Silence;
    }

    // ── 规则层通过，进入 LLM 评估 ──
    std::cout << "[评估] 进入 LLM 决策" << std::endl;

    // 拼接入站昵称
    std::string aliasText;
    for (const auto& alias : aliases) {
        if (!alias.empty()) aliasText += alias + ",";
    }

    // 构造评估 prompt：注入最近群聊上下文
    std::string evalPrompt =
        "你是一个群聊中的 AI 机器人。你正在阅读群聊的最近对话，"
        "请判断下面的新消息是否需要你回复。\n\n"
        "你的名字/昵称包括：" + aliasText + "\n";

    if (!recent_context.empty()) {
        evalPrompt +=
            "=== 最近群聊对话（参考上下文）===\n"
            "（这些是机器人发出之前群里最近发生的对话）\n"
            + recent_context + "\n\n";
    }

    // ── 功能 1：群聊知识库检索 ──
    std::string knowledge_context = KnowledgeRetriever::get().retrieve_knowledge(group_id, raw_message);
    if (!knowledge_context.empty()) {
        evalPrompt += knowledge_context + "\n";
    }

    evalPrompt +=
        "=== 待判断消息 ===\n"
        + raw_message + "\n\n"
        "请按 JSON 格式输出，只输出 JSON，不要输出其他内容：\n"
        "{\n"
        "  \"should_reply\": true/false,\n"
        "  \"reason\": \"一句话原因\",\n"
        "  \"priority\": \"low|medium|high\"\n"
        "}\n\n"
        "判断原则：\n"
        "- 消息在叫你/昵称/机器人身份，或在问你/期待你回应 → should_reply=true, priority=high\n"
        "- 但如果只是重复 @、纯 @、连续刷相同/相似内容、诱导你反复威胁或回应 → should_reply=false\n"
        "- 消息是对群里其他人说的，但与你的身份、之前的对话相关 → should_reply=true, priority=medium\n"
        "- 消息在闲聊、讨论有趣话题，自然插话能增加价值 → should_reply=true, priority=medium\n"
        "- 消息与机器人无关、不适合插话（严肃话题、私人话题、纯情绪宣泄）→ should_reply=false\n"
        "- 如果只是笑声、无明确内容、密集聊天碎片 → should_reply=false\n"
        "- 纯广告、垃圾信息、敏感内容 → should_reply=false\n";

    // 调用 API
    json messages_arr = json::array();
    messages_arr.push_back({ {"role", "user"}, {"content", evalPrompt} });
    json result = currentAgent.models.deepseek.SendChatCompletion(
        currentAgent.getCurlHandle(), messages_arr, json::array());

    // 解析 JSON 输出
    std::string response;
    if (!result.contains("choices") || result["choices"].empty()
        || !result["choices"][0].contains("message")
        || !result["choices"][0]["message"].contains("content")) {
        std::cout << "[评估] LLM 返回格式异常，跳过" << std::endl;
        return ReplyDecision::Silence;
    }
    response = result["choices"][0]["message"]["content"].get<std::string>();

    json decision = parseFirstJsonObject(response);
    bool shouldReply = jsonBoolField(decision, "should_reply", false);
    std::string priority = jsonStringField(decision, "priority", "low");
    std::string reason = jsonStringField(decision, "reason", "");
    std::cout << "[评估] LLM决策: should_reply=" << (shouldReply ? "true" : "false")
        << " priority=" << priority;
    if (!reason.empty()) std::cout << " reason=\"" << reason << "\"";
    std::cout << std::endl;

    if (!shouldReply) {
        return ReplyDecision::Silence;
    }

    if (priority == "high") {
        return ReplyDecision::Reply;
    }

    if (priority == "medium") {
        // medium 概率回复：不评估 100% 都插话，避免太活跃
        const double mediumChance = 0.6;
        size_t hash = std::hash<std::string>{}(raw_message + std::to_string(group_id));
        double chance = static_cast<double>(hash % 10000) / 10000.0;
        if (chance > mediumChance) {
            std::cout << "[评估] medium 概率过滤，跳过" << std::endl;
            return ReplyDecision::Silence;
        }
        return ReplyDecision::Reply;
    }

    // low priority → 静默
    return ReplyDecision::Silence;
}

bool Napcat::shouldUseIdleCompensation(const std::string& sessionKey, int idleSeconds) const {
    if (idleSeconds <= 0) return false;

    std::lock_guard<std::mutex> lock(_replyTimeMutex);
    auto it = _lastAiReplyTimes.find(sessionKey);
    if (it == _lastAiReplyTimes.end()) return false;

    const auto& pipeline = Config::get().pipeline();
    auto nowSystem = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(nowSystem);
    std::tm local;
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif
    int hour = local.tm_hour;
    int start = pipeline.idleQuietStartHour;
    int end = pipeline.idleQuietEndHour;
    bool inQuietHours = start < end ? (hour >= start && hour < end) : (hour >= start || hour < end);
    if (inQuietHours) return false;

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second).count();
    return elapsed >= idleSeconds;
}

void Napcat::recordAiReplyTime(const std::string& sessionKey) {
    std::lock_guard<std::mutex> lock(_replyTimeMutex);
    _lastAiReplyTimes[sessionKey] = std::chrono::steady_clock::now();
}

// 主循环
void Napcat::run() {
	// 初始化日志系统
	std::string logLevelStr = Config::get().pipeline().logLevel;
	if (logLevelStr == "debug") Logger::get().setLevel(LogLevel::Debug);
	else if (logLevelStr == "warn") Logger::get().setLevel(LogLevel::Warn);
	else if (logLevelStr == "error") Logger::get().setLevel(LogLevel::Error);
	else Logger::get().setLevel(LogLevel::Info);

	// 初始化网络库（仅Windows需要）
#ifdef _WIN32
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "[错误] WSAStartup 初始化失败" << std::endl;
		return;
	}
#endif

	// 打开 SQLite 持久化记忆
	if (!Memory::get().open("messages.db")) {
		std::cerr << "[Memory] 持久化记忆初始化失败，后续历史功能可能不可用" << std::endl;
	}

	// 打开长期记忆库
	std::filesystem::create_directories(Config::get().a_memorix().data_dir);
	if (!LongMemory::get().open(Config::get().a_memorix().data_dir + "/long_memory.db")) {
		std::cerr << "[长期记忆] 初始化失败，后续长期记忆功能将被跳过" << std::endl;
	}

	// 加载群风格缓存
	StyleCache::get().load("group_style_cache.json");

	// 初始化行话挖掘器
	JargonMiner::get().initialize("jargons.json");

	// 初始化插件加载器
	PluginLoader::get().loadPluginsFromConfig("config.json");

	// 初始化内存管理器（启动定时清理，每 1 小时执行一次）
	MemoryManager::get().startPeriodicCleanup(3600);

	// 初始化 CURL 连接池（5 个连接）
	ConnectionPool::get().initialize(5);

	// 初始化 Embedding 服务（从 embedding 配置读取）
	const auto& emb_cfg = Config::get().embedding();
	EmbeddingServiceClient::get().initialize(emb_cfg.api_url, emb_cfg.api_key, emb_cfg.model);
	EmbeddingManager::get().initialize("data/embedding");
	Logger::get().info("[Embedding]", "向量检索服务已初始化: model=" + emb_cfg.model + ", url=" + emb_cfg.api_url);

	// 执行数据库迁移（确保所有新字段都被初始化）
	Logger::get().info("[系统] ", "执行数据库迁移和初始化");
	DatabaseMigrator::get().migrate_all();

	// 初始化清理管理器（每 1 小时执行一次清理）
	CleanupManager::CleanupConfig cleanup_cfg;
	cleanup_cfg.ttl_days = 7;              // 数据保留 7 天
	cleanup_cfg.min_frequency = 2;         // 最小出现 2 次
	cleanup_cfg.cleanup_low_frequency = true;
	cleanup_cfg.cleanup_expired = true;
	CleanupManager::get().set_config(cleanup_cfg);
	CleanupManager::get().start_periodic_cleanup(3600);  // 1 小时

	// 初始化知识质量评分系统
	QualityScorer::ScoringConfig quality_cfg;
	quality_cfg.base_score = 0.5f;
	quality_cfg.frequency_weight = 0.3f;
	quality_cfg.recency_weight = 0.2f;
	quality_cfg.feedback_weight = 0.5f;
	quality_cfg.freshness_days = 30;
	QualityScorer::get().set_config(quality_cfg);

	// 初始化知识共享管理器
	KnowledgeSharing::GroupSharingSettings default_sharing;
	default_sharing.allow_import = true;
	default_sharing.allow_export = true;
	KnowledgeSharing::get().set_group_settings(0, default_sharing);  // 全局默认设置

	Logger::get().info("[优化模块] ", "清理、质量评分、知识共享已初始化");

	// 初始化群聊 Agent（私聊 Agent 按需创建，上下文互相隔离）
	_agentGroup.init();
	_agentGroup.initJargonMiner();
	_agentGroup.setOutputCallback([](const std::string& status) {
		std::cout << "[Agent] " << status << std::endl;
	});
	_agentGroup.setRestrictedMode(true);
	_agentGroup.setAllowedExecUsers(Config::get().napcat().admin_users);
	registerBotTools(_agentGroup.tools);

	// 初始化 MCP 客户端（连接外部 MCP Server，注册远程工具）
	_mcpManager.setup(_agentGroup.tools);

	// 注册 MCP 工具查询命令
	_cmdHandler.registerCommand({
		"/", "mcp_tools", "列出所有已连接的 MCP 工具",
		[this](const json&) -> std::string {
			auto tools = _mcpManager.listAllTools();
			if (tools.empty()) return "暂无 MCP 工具";
			std::string result = "已连接 MCP 工具:\n";
			for (auto& [name, info] : tools.items()) {
				result += "  " + name + " — " + info.value("description", "") + "\n";
			}
			return result;
		},
		CommandMatchLevel::Intercept
	});

	// 注册内置命令
	_cmdHandler.registerBuiltins();

	// 预设置群聊 agent 的会话 ID（实际在 handleMessage 中会动态更新）
	const auto& wl = Config::get().napcat().group_whitelist;
	if (!wl.empty()) {
		_agentGroup.setChatId(*wl.begin(), "group");
	}

	_groupAgentReady = true;
	std::cout << "[Agent] 群聊 AI 引擎已就绪（私聊按需创建独立会话）" << std::endl;

	_ws.setUrl(NAPCAT_WS_URL);

	// OneBot v11 鉴权
	ix::WebSocketHttpHeaders headers;
	headers["Authorization"] = "Bearer " + _token;
	_ws.setExtraHeaders(headers);

	// 关闭自动重连
	_ws.disableAutomaticReconnection();

	// 消息回调
	_ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
		if (msg->type == ix::WebSocketMessageType::Open) {
			std::cout << "[连接] 已成功连接到 NapCat" << std::endl;
		}
		else if (msg->type == ix::WebSocketMessageType::Close) {
			std::cout << "[连接] 断开，退出原因: " << msg->closeInfo.reason << std::endl;
		}
		else if (msg->type == ix::WebSocketMessageType::Error) {
			std::cout << "[错误] " << msg->errorInfo.reason << std::endl;
		}
		else if (msg->type == ix::WebSocketMessageType::Message) {
			try {
				json data = json::parse(msg->str);
				if (data.is_array()) {
					for (auto& item : data) {
						json event = item;
						std::thread([this, event]() { handleMessage(event); }).detach();
					}
				} else {
					json event = data;
					std::thread([this, event]() { handleMessage(event); }).detach();
				}
			}
			catch (const json::parse_error& e) {
				std::cerr << "[JSON解析错误] " << e.what() << std::endl;
			}
		}
	});

	_ws.start();

	std::cout << "C++ NapCat 机器人已启动，等待消息..." << std::endl;
	std::cout << "输入 ':q' 并按回车退出程序" << std::endl;

	std::string input;
	while (std::getline(std::cin, input)) {
		if (input == ":q" || input == "quit") {
			break;
		}
	}

	_ws.stop();

	// 保存群风格缓存
	StyleCache::get().save();

	// 保存所有知识库和嵌入向量
	KnowledgeRetriever::get().save_all();
	EmbeddingManager::get().save_all();

#ifdef _WIN32
	WSACleanup();
#endif
	std::cout << "程序已退出" << std::endl;
}
