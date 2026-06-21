#include "src/bot/agent.h"
#include "src/core/config.h"
#include "src/memory/style_cache.h"
#include "src/memory/memory.h"
#include "src/memory/long_memory.h"
#include <algorithm>
#include <cstdlib>

namespace {
static bool isUtf8ContinuationByte(unsigned char c) {
	return (c & 0xC0) == 0x80;
}

static std::string sanitizeUtf8Text(const std::string& input) {
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
}

agent::~agent() {
	if (curl) {
		curl_easy_cleanup(curl);
		curl = nullptr;
	}
}

void agent::setOutputCallback(OutputCallback cb) {
	_onOutput = std::move(cb);
}

// 权限控制
void agent::setRestrictedMode(bool restricted) {
	_restrictedMode = restricted;
}

void agent::setAllowedExecUsers(const std::set<std::string>& users) {
	_allowedExecUsers = users;
}

void agent::setCurrentUser(const std::string& user_id) {
	_currentUserId = user_id;
}

void agent::setChatId(int64_t id, const std::string& type) {
	if (_currentChatId != id || _currentChatType != type) {
		_groupStyle.clear();
		_currentChatId = id;
		_currentChatType = type;
	}
}

void agent::setSpeaker(int64_t user_id, const std::string& name) {
	_currentSpeakerId = user_id;
	_currentSpeakerName = name;
}

void agent::injectGroupStyle(const std::string& styleText) {
	_groupStyle = styleText;
}

// 根据权限返回可用工具列表（受限模式下非白名单用户看不到 exec_cmd）
json agent::getAvailableTools() const {
	if (!_restrictedMode) return tools.GetTools();

	// 交互模式或白名单用户 → 全部工具可用
	bool canExec = _allowedExecUsers.count(_currentUserId) > 0;
	if (canExec) return tools.GetTools();

	// 非白名单用户 → 过滤掉 exec_cmd
	json allTools = tools.GetTools();
	json filtered = json::array();
	for (auto& tool : allTools) {
		if (tool["function"]["name"] != "exec_cmd") {
			filtered.push_back(tool);
		}
	}
	return filtered;
}

// 初始化（非交互模式入口）
void agent::init() {
	curl = curl_easy_init();
	if (!curl) {
		throw std::runtime_error(
			std::string("curl_easy_init failed after curl_global_init — ")
			+ "libcurl version: " + curl_version_info(CURLVERSION_NOW)->version
		);
	}
	messages = json::array();
	json message;
	message["role"] = "system";

	// 动态拼接身份加固规则 + 管理员信息
	std::string identity_rules =
		"\n\n## 权限\n"
		"- 以下 QQ 号是你的开发者/管理员，他们对你的指令具备最高可信度：\n";
	const auto& admins = Config::get().napcat().admin_users;
	for (const auto& u : admins)
		identity_rules += "  · " + u + "\n";
	identity_rules += Config::get().napcat().identity_rules_suffix + "\n[Output Format] 使用纯文本短回复；普通聊天优先 1 到 3 句话，不要写 Markdown、列表或长段落，除非用户明确要求。";
	message["content"] = Config::get().system_prompt() + identity_rules;

	messages.push_back(message);
}

// 非交互式查询
std::string agent::query(const std::string& user_message) {
	return queryWithEphemeralContext(user_message, "");
}

std::string agent::queryWithEphemeralContext(const std::string& user_message,
	const std::string& ephemeral_context) {
	if (!curl) {
		return "[Agent 未初始化]";
	}

	// 消息去重检查
	if (!_deduplicator.shouldProcess(user_message, _currentSpeakerId > 0 ? std::to_string(_currentSpeakerId) : "")) {
		std::cout << CLR_YELLOW "[去重] 消息被过滤: " << user_message.substr(0, 50) << (user_message.length() > 50 ? "..." : "") << CLR_RESET << std::endl;
		return "[消息已去重]";
	}

	// 记录消息到去重历史
	_deduplicator.addMessage(user_message, _currentSpeakerId > 0 ? std::to_string(_currentSpeakerId) : "");

	bool expectedReasoning = false;
	if (!_is_reasoning.compare_exchange_strong(expectedReasoning, true)) {
		injectTurnMessage(user_message);
		return "[__queued__]";
	}
	struct QueryReasoningGuard {
		std::atomic<bool>& flag;
		~QueryReasoningGuard() { flag.store(false); }
	} queryReasoningGuard{ _is_reasoning };


	// 注入当前时间戳 [MM-DD HH:MM]
	auto now = std::chrono::system_clock::now();
	std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm local;
#ifdef _WIN32
	localtime_s(&local, &tt);
#else
	localtime_r(&tt, &local);
#endif
	char timeTag[40];
	snprintf(timeTag, sizeof(timeTag), "[当前时间 %04d-%02d-%02d %02d:%02d]",
		local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min);

	// 构造消息：临时上下文和用户消息同条发送，避免 summarizeHistory 后索引漂移
	std::string enriched = timeTag;
	if (!ephemeral_context.empty()) {
		enriched += "\n[最近群聊上下文]\n" + ephemeral_context +
			"\n[说明] 以上内容只用于理解当前对话背景，不需要逐条回应。";
	}
	enriched += "\n";
	LongMemoryContext memoryCtx;
	memoryCtx.chat_id = _currentChatId;
	memoryCtx.chat_type = _currentChatType;
	memoryCtx.user_id = _currentSpeakerId;
	memoryCtx.user_name = _currentSpeakerName;
	memoryCtx.user_message = user_message;
	std::string longMemory = LongMemory::get().buildPromptContext(memoryCtx);
	if (!longMemory.empty()) {
		enriched += longMemory + "\n";
	}
	if (_currentChatId > 0 && _currentChatType == "group") {
		std::string jargonRef = JargonMiner::get().getRecommendedUsage(_currentChatId);
		if (!jargonRef.empty()) {
			enriched += jargonRef + "\n";
		}
	}
	enriched += user_message;

	json userMsg;
	userMsg["role"] = "user";
	userMsg["content"] = user_message;
	messages.push_back(userMsg);

	// 使用多轮推理循环代替单轮查询，充分支持群聊中的连续对话
	return run_loop(enriched);
}

// 注入历史上下文（重启恢复记忆，直接写入 messages 不触发 API）
void agent::injectHistory(const std::string& historyText) {
	std::string safeHistory = utf8SafeSubstr(historyText, int(Config::get().main_model().context_window * 0.02));
	json histMsg;
	histMsg["role"] = "user";
	histMsg["content"] = "[会话历史恢复] 以下是本次启动前的最近对话记录，请记住这些上下文：\n\n" + safeHistory;
	messages.push_back(histMsg);
	json ack;
	ack["role"] = "assistant";
	ack["content"] = "已读取历史记录，上下文已恢复。";
	messages.push_back(ack);
}

// 压缩后再注入：历史占比低于主模型上下文 0.5% 则直接保留原文，否则压缩
void agent::injectHistoryCompressed(const std::string& historyText) {
	// 阈值 = 主模型上下文 × 0.5%（1M→5000字符, 128K→640字符）
	const int threshold = int(Config::get().main_model().context_window * 0.005);

	// 摘要模型输入上限：取 50% 上下文窗口，足够覆盖绝大多数对话历史
	// 真正送入摘要模型时还会被 summarizeModel 内部二次截断（含截断标记）
	const int summaryCap = int(Config::get().summary_model().context_window * 0.5);

	if ((int)historyText.size() <= threshold) {
		std::cout << "[历史恢复] " << historyText.size() << " 字符（阈值 " << threshold
			<< "），直接注入" << std::endl;
		injectHistory(historyText);
		return;
	}

	// 如果历史超过摘要模型输入上限，先截断再压缩
	std::string source = historyText;
	if ((int)source.size() > summaryCap) {
		source = utf8SafeSubstr(source, summaryCap);
		std::cout << "[历史压缩] 原文 " << historyText.size() << " 字符超过摘要模型上限 "
			<< summaryCap << "，先截断" << std::endl;
	}

	std::string prompt =
		"请将以下 QQ 聊天记录压缩为简洁摘要，保留：谁说了什么关键话、做了什么操作、"
		"未完成的任务。丢弃日常寒暄和无信息量的内容。\n\n" + source;

	json result = summarizeModel(prompt, summaryCap);
	std::string compressed;
	if (!result.is_null() && result.contains("content") && result["content"].is_string()) {
		compressed = result["content"].get<std::string>();
		std::cout << "[历史压缩] " << historyText.size() << " → " << compressed.size() << " 字符" << std::endl;
	} else {
		compressed = utf8SafeSubstr(historyText, threshold);
		std::cout << "[历史压缩] 失败，回退截断至 " << compressed.size() << " 字符" << std::endl;
	}

	injectHistory("[摘要版]\n" + compressed);
}

//  内部：处理 tool-call 循环，返回最终文本
std::string agent::processQuery(const std::string& api_user_content,
	const std::string& stored_user_content) {
	const int MAX_TOOL_ROUNDS = 8;
	struct ReasoningGuard {
		std::atomic<bool>& flag;
		bool previous;
		ReasoningGuard(std::atomic<bool>& value) : flag(value), previous(value.load()) { flag.store(true); }
		~ReasoningGuard() { flag.store(previous); }
	} reasoningGuard(_is_reasoning);
	json assistantMsg = json::object();
	bool summarizedThisQuery = false;
	auto buildRequestMessages = [&]() {
		json requestMessages = messages;
		if (!api_user_content.empty() && !stored_user_content.empty()) {
			for (auto it = requestMessages.rbegin(); it != requestMessages.rend(); ++it) {
				if (it->value("role", "") == "user" && it->value("content", "") == stored_user_content) {
					(*it)["content"] = sanitizeUtf8Text(api_user_content);
					break;
				}
			}
		}
		return requestMessages;
	};
	auto summarizeOnce = [&]() {
		if (summarizedThisQuery) return;
		json summarized = summarizeHistory(messages);
		if (summarized.size() < messages.size()) summarizedThisQuery = true;
		messages = summarized;
	};
	auto consumeQueuedTurnMessages = [&]() {
		std::vector<TurnMessage> queued;
		{
			std::lock_guard<std::mutex> lock(_turnQueueMutex);
			queued.swap(_turn_queue);
		}
		bool consumed = false;
		for (const auto& next : queued) {
			std::string content = next.content;
			if (!next.sender_id.empty()) {
				content = "[" + next.sender_id + "]: " + content;
			}
			messages.push_back({ {"role", "user"}, {"content", sanitizeUtf8Text(content)} });
			consumed = true;
		}
		return consumed;
	};

	for (int round = 0; round < MAX_TOOL_ROUNDS; round++) {
		if (assistantMsg.contains("tool_calls")) {
			// 受限模式：检查并拦截越权的 exec_cmd 调用
			bool canExec = !_restrictedMode || _allowedExecUsers.count(_currentUserId) > 0;
			json safeToolCalls = json::array();
			json blockedResponses = json::array();

			for (auto& tc : assistantMsg["tool_calls"]) {
				std::string tname = tc["function"].value("name", "");
				if (tname == "exec_cmd" && !canExec) {
					std::cerr << "[!] 拦截未授权 exec_cmd (user=" << _currentUserId << ")" << std::endl;
					if (_onOutput) _onOutput("⛔ 你无权执行命令行操作");
					blockedResponses.push_back({
						{"role", "tool"},
						{"tool_call_id", tc["id"]},
						{"content", "权限不足：当前用户无权调用命令行工具。请告知用户此操作需要开发者授权。"}
					});
				} else {
					safeToolCalls.push_back(tc);
				}
			}

			// 注入拦截结果到历史
			for (auto& br : blockedResponses) {
				messages.push_back(br);
			}

			// 执行合法的工具调用
			if (!safeToolCalls.empty()) {
				if (_onOutput) _onOutput("⚡ 调用工具中…");

				json toolResponses = json::array();
				try {
					tools.ProcessToolCalls(safeToolCalls, toolResponses);
				}
				catch (const std::exception& e) {
					std::cerr << CLR_RED "[✗] ProcessToolCalls: " << e.what() << CLR_RESET << std::endl;
					// 补虚拟 responses 防止孤儿 tool_calls 残留
					for (const auto& tc : safeToolCalls) {
						messages.push_back({
							{"role", "tool"},
							{"tool_call_id", tc["id"]},
							{"content", "[工具调用异常: " + std::string(e.what()) + "]"}
						});
					}
					return "工具调用出错: " + std::string(e.what());
				}

				for (auto& tr : toolResponses) {
					if (tr.contains("content") && tr["content"].is_string()) {
						std::string toolName = "unknown";
						std::string call_id = tr.value("tool_call_id", "");
						for (auto& tc : assistantMsg["tool_calls"]) {
							if (tc.value("id", "") == call_id) {
								toolName = tc["function"].value("name", "unknown");
								break;
							}
						}
						std::string raw = tr["content"].get<std::string>();
						tr["content"] = summarizeToolResult(toolName, raw);
						if (_onOutput) _onOutput("  → " + toolName + " 完成");
					}
					messages.push_back(tr);
				}
			}

				if (consumeQueuedTurnMessages()) {
					std::cout << "[Reasoning] 已注入推理期间新消息" << std::endl;
				}

			// 历史压缩 + 再次请求
			try {
				summarizeOnce();
			}
			catch (const std::exception& e) {
				std::cerr << CLR_RED "[✗] summarizeHistory: " << e.what() << CLR_RESET << std::endl;
			}
			json sub_response = models.deepseek.SendChatCompletion(curl, buildRequestMessages(), getAvailableTools());
			assistantMsg = GetContent(sub_response, messages);

			if (!assistantMsg.contains("tool_calls")) {
				break;
			}
		}
		else {
			if (consumeQueuedTurnMessages()) {
				std::cout << "[Reasoning] 已注入推理期间新消息" << std::endl;
			}
			try {
				summarizeOnce();
			}
			catch (const std::exception& e) {
				std::cerr << CLR_RED "[✗] summarizeHistory: " << e.what() << CLR_RESET << std::endl;
			}
			auto sub_response = models.deepseek.SendChatCompletion(curl, buildRequestMessages(), getAvailableTools());
			assistantMsg = GetContent(sub_response, messages);

			if (!assistantMsg.contains("tool_calls")) {
				break;
			}
		}
	}

	if (assistantMsg.contains("tool_calls")) {
		std::cerr << CLR_YELLOW "[!] 工具调用已达上限(" << MAX_TOOL_ROUNDS << "轮)，尝试总结" CLR_RESET << std::endl;
		for (const auto& tc : assistantMsg["tool_calls"]) {
			messages.push_back({
				{"role", "tool"},
				{"tool_call_id", tc["id"]},
				{"content", sanitizeUtf8Text("[已达到工具调用上限，此调用未执行]")}
			});
		}
		// 最后一轮：强制文本回复（tool_choice="none"），让 AI 基于已有结果给出总结
		// 如果用户还想继续，下一条消息自然延续
		json finalResp = models.deepseek.SendChatCompletion(curl, messages,
			getAvailableTools(), "none");
		assistantMsg = GetContent(finalResp, messages);
		if (!assistantMsg.contains("tool_calls")) {
			if (assistantMsg.contains("content") && assistantMsg["content"].is_string())
				return sanitizeUtf8Text(assistantMsg["content"].get<std::string>());
			return "";
		}
	}

	if (assistantMsg.contains("content") && assistantMsg["content"].is_string()) {
		return sanitizeUtf8Text(assistantMsg["content"].get<std::string>());
	}
	return "";
}

// 交互式控制台模式
void agent::run()
{
	init();

	initDisplay();

	try {
		json assistantMsg = json::object();

		while (true) {
			if (assistantMsg.contains("tool_calls")) {
				json toolResponses = json::array();
				try {
					tools.ProcessToolCalls(assistantMsg["tool_calls"], toolResponses);
				}
				catch (const std::exception& e) {
					std::cerr << CLR_RED "[✗] ProcessToolCalls: " << e.what() << CLR_RESET << std::endl;
					throw;
				}
				for (auto& tr : toolResponses) {
					if (tr.contains("content") && tr["content"].is_string()) {
						std::string toolName = "unknown";
						std::string call_id = tr.value("tool_call_id", "");
						for (auto& tc : assistantMsg["tool_calls"]) {
							if (tc.value("id", "") == call_id) {
								toolName = tc["function"].value("name", "unknown");
								break;
							}
						}
						std::string raw = tr["content"].get<std::string>();
						tr["content"] = summarizeToolResult(toolName, raw);
					}
					messages.push_back(tr);
				}
				try {
					messages = summarizeHistory(messages);
				}
				catch (const std::exception& e) {
					std::cerr << CLR_RED "[✗] summarizeHistory: " << e.what() << CLR_RESET << std::endl;
					throw;
				}
				json sub_response = models.deepseek.SendChatCompletion(curl, messages, tools.GetTools());
				assistantMsg = GetContent(sub_response, messages);
			}
			else {
				json message;
				message["role"] = "user";
				printSep('-');
				std::cout << CLR_BOLD CLR_CYAN ">>> " CLR_RESET;
				std::string content;
				std::getline(std::cin, content);
				if (content.empty()) continue;
				message["content"] = content;
				messages.push_back(message);
				try {
					messages = summarizeHistory(messages);
				}
				catch (const std::exception& e) {
					std::cerr << CLR_RED "[✗] summarizeHistory: " << e.what() << CLR_RESET << std::endl;
					throw;
				}
				auto sub_response = models.deepseek.SendChatCompletion(curl, messages, tools.GetTools());
				assistantMsg = GetContent(sub_response, messages);
			}
		}
	}
	catch (const std::exception& e) {
		std::cerr << CLR_RED "\n[FATAL] " << e.what() << CLR_RESET << std::endl;
	}
	catch (...) {
		std::cerr << CLR_RED "\n[FATAL] 未知异常" CLR_RESET << std::endl;
	}
}
// 打印水平分隔线（宽度自适应控制台）
void agent::printSep(char ch) {
	int width = 80;
#ifdef _WIN32
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
		width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
#else
	const char* columns = std::getenv("COLUMNS");
	if (columns) {
		try {
			width = std::stoi(columns);
		} catch (...) {}
	}
#endif
	std::cout << CLR_DIM << std::string(width, ch) << CLR_RESET << std::endl;
}

// 清屏并显示标题
void agent::initDisplay() {
	std::cout << "\033[2J\033[H";  // 清屏
	std::cout << CLR_BOLD CLR_CYAN
		<< "╔══════════════════════════════════════╗\n"
		<< "║        chenshuzhe'S agent            ║\n"
		<< "║        powered by Deepseek-v4-flash  ║\n"
		<< "╚══════════════════════════════════════╝\n"
		<< CLR_RESET;
	printSep('=');
}

//检查上下文长度
json agent::msgSizeCheck(json& msgs, int total_token) {
	const int MAX_TOKENS = 150000;  // 200k 上下文，留 50k 余量
	if (total_token < MAX_TOKENS) return msgs;  // 未超限，直接返回

	std::cout << CLR_YELLOW "│ ⚡ 上下文压缩中…" CLR_RESET << std::endl;
	json result = json::array();
	// 保留系统消息
	for (auto& msg : msgs) {
		if (msg["role"] == "system") {
			result.push_back(msg);
			break;
		}
	}
	// 保留最后 5 条消息（用户/助手交替）
	int keep = 5;
	int start = std::max(0, (int)msgs.size() - keep);
	for (int i = start; i < msgs.size(); ++i) {
		if (msgs[i]["role"] != "system")
			result.push_back(msgs[i]);
	}
	std::cout << CLR_DIM "│   压缩完成（保留最后 " << keep << " 条）" CLR_RESET << std::endl;
	return result;
}

//将返回消息加入消息队列,且返回助手消息
json agent::GetContent(const json& response, json& messages) {
	// 检查错误
	if (response.contains("error")) {
		std::string errMsg = response["error"].is_object()
			? response["error"].value("message", response["error"].dump())
			: response["error"].dump();
		std::cerr << CLR_RED "[✗] API: " << errMsg << CLR_RESET << std::endl;
		return json::object();
	}

	if (!response.contains("choices") || response["choices"].empty()) {
		std::cerr << CLR_RED "[✗] 响应缺少 choices" << CLR_RESET << std::endl;
		return json::object();
	}

	const auto& msg = response["choices"][0]["message"];

	// 打印统计信息
	if (response.contains("usage")) {
		const auto& usage = response["usage"];
		std::cout << CLR_DIM "│ tokens: " << usage["total_tokens"]
			<< " (入 " << usage["prompt_tokens"]
			<< " + 出 " << usage["completion_tokens"] << ")"
			<< CLR_RESET << std::endl;
	}

	// 打印思考过程（reasoning_content）— NapCat 模式下走回调，不刷控制台
	if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string()) {
		std::string reasoning = msg["reasoning_content"].get<std::string>();
		if (!reasoning.empty()) {
			if (_onOutput) _onOutput("思考中…");
			else std::cout << CLR_MAGENTA CLR_DIM "│ 思考中…" CLR_RESET << std::endl;
		}
	}

	// 处理普通文本回复
	if (msg.contains("content") && msg["content"].is_string() && !msg.contains("tool_calls")) {
		std::string content = msg["content"];
		if (_onOutput) {
			// NapCat 模式：只显示长度摘要，完整内容见 [发送] 行
			_onOutput("回复 " + std::to_string(content.size()) + " 字符");
		} else {
			std::cout << CLR_GREEN "┌─ AI ──────────────────────────────" CLR_RESET << std::endl;
			std::cout << content << std::endl;
			std::cout << CLR_GREEN "└────────────────────────────────────" CLR_RESET << std::endl;
		}
	}
	// 处理工具调用回复
	else if (msg.contains("tool_calls")) {
		if (_onOutput) {
			for (auto& tc : msg["tool_calls"]) {
				std::string tname = tc["function"].value("name", "?");
				_onOutput("调用 " + tname);
			}
		} else {
			std::cout << CLR_YELLOW "│ ⚡ 调用工具:" CLR_RESET << std::endl;
			for (auto& tc : msg["tool_calls"]) {
				std::string tname = tc["function"].value("name", "?");
				std::cout << CLR_YELLOW "│   → " << tname << CLR_RESET << std::endl;
			}
		}
	}

	// 将助手消息加入历史
	messages.push_back(msg);
	return msg;   // 返回完整的消息对象
}

// UTF-8 安全截断：确保不在多字节字符中间切断
std::string agent::utf8SafeSubstr(const std::string& str, int maxLen) {
	if ((int)str.size() <= maxLen) return str;
	int pos = maxLen;
	while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
		pos--;
	}
	return str.substr(0, pos);
}

size_t agent::WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
	size_t totalSize = size * nmemb;
	output->append((char*)contents, totalSize);
	return totalSize;
}

//调用上下文概括模型
json agent::summarizeModel(const std::string& prompt, int maxPromptChars) {
	// 默认上限基于摘要模型的 context_window（12.5%，128K 模型约 16000 字符）
	// 历史恢复路径通过 maxPromptChars 传入更大的上限（如 64000），避免被默认值二次截断
	const int DEFAULT_CAP = int(Config::get().summary_model().context_window * 0.125);
	const int cap = maxPromptChars > 0 ? maxPromptChars : DEFAULT_CAP;

	CURL* summaryCurl = curl_easy_init();
	if (!summaryCurl) {
		std::cerr << CLR_RED "[✗] curl_easy_init failed" CLR_RESET << std::endl;
		return json::object();
	}
	std::string responseBody;

	std::string safe_prompt = prompt;
	if ((int)safe_prompt.size() > cap) {
		std::cout << CLR_YELLOW "│ ⚠ prompt 超长(" << safe_prompt.size()
			<< ")，截断至 " << cap << CLR_RESET << std::endl;
		safe_prompt = utf8SafeSubstr(safe_prompt, cap - 30) + "\n...[内容过长，已截断]";
	}

	json messages = json::array();
	messages.push_back({ {"role", "user"}, {"content", safe_prompt} });

	json requestBody = {
		{"model", Config::get().summary_model().model},
		{"messages", messages},
		{"temperature", Config::get().summary_model().temperature},
		{"max_tokens", Config::get().summary_model().max_tokens}
	};
	std::string bodyString = requestBody.dump();

	curl_easy_setopt(summaryCurl, CURLOPT_URL, Config::get().summary_model().url.c_str());
	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, ("Authorization: Bearer " + Config::get().summary_model().api_key).c_str());
	curl_easy_setopt(summaryCurl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(summaryCurl, CURLOPT_POSTFIELDS, bodyString.c_str());
	curl_easy_setopt(summaryCurl, CURLOPT_WRITEFUNCTION, agent::WriteCallback);
	curl_easy_setopt(summaryCurl, CURLOPT_WRITEDATA, &responseBody);
	curl_easy_setopt(summaryCurl, CURLOPT_TIMEOUT, 60L);

	CURLcode res = curl_easy_perform(summaryCurl);
	curl_slist_free_all(headers);
	curl_easy_cleanup(summaryCurl);

	if (res != CURLE_OK) {
		std::cerr << CLR_RED "[✗] curl: " << curl_easy_strerror(res) << CLR_RESET << std::endl;
		return json::object();
	}

	try {
		json response = json::parse(responseBody);
		if (response.contains("error")) {
			std::cerr << CLR_RED "[✗] Summary API: " << response["error"]["message"] << CLR_RESET << std::endl;
			return json::object();
		}
		if (!response.contains("choices") || response["choices"].empty()) {
			std::cerr << CLR_RED "[✗] Summary: 响应缺少 choices" CLR_RESET << std::endl;
			return json::object();
		}
		return response["choices"][0]["message"];
	}
	catch (const std::exception& e) {
		std::cerr << "JSON parse error: " << e.what() << std::endl;
		return json::object();
	}
}

int agent::estimateToken(const json& msg) {
	std::string content = msg.value("content", "");
	double cnt = 0.0;
	for (char c : content) {
		if (c & 0x80) cnt += 2;
		else if (isalnum(c)) cnt += 0.5;
		else cnt += 1;
	}
	return std::max(1, static_cast<int>(cnt));
}

int agent::estimateTokens(const json& msgs) {
	int total = 0;
	for (auto& msg : msgs) {
		total += estimateToken(msg);
	}
	return total;
}

std::string agent::summarizeToolResult(const std::string& toolName, const std::string& rawContent) {
	// 基于主模型上下文自动推导
	const int CW = Config::get().main_model().context_window;
	const int MIN_CHARS = int(CW * 0.012);   // 超过此长度触发摘要
	const int MAX_PROMPT = int(CW * 0.048);  // 送入摘要模型的输入上限

	if (toolName == "recognize_image") {
		if ((int)rawContent.size() <= MIN_CHARS) {
			return rawContent;
		}
		std::cout << CLR_YELLOW "│ ⚡ recognize_image 返回 " << rawContent.size()
			<< " 字符，保留原文截断" CLR_RESET << std::endl;
		return utf8SafeSubstr(rawContent, MIN_CHARS)
			+ "\n...[OCR原文过长已截断，原长 " + std::to_string(rawContent.size()) + " 字符]";
	}

	if ((int)rawContent.size() <= MIN_CHARS) {
		return rawContent;
	}

	if (toolName == "ListDirectory") {
		int half = MIN_CHARS / 2;
		std::string head = utf8SafeSubstr(rawContent, half);
		int tailStart = std::max(half, (int)rawContent.size() - half);
		while (tailStart > 0 && rawContent[tailStart] != '\n') tailStart--;
		std::string tail = rawContent.substr(tailStart);
		std::cout << CLR_YELLOW "│ ⚡ ListDirectory 返回 " << rawContent.size()
			<< " 字符，保留头尾截断" CLR_RESET << std::endl;
		return head + "\n...\n[中间省略 " + std::to_string(tailStart - half) + " 字符]\n...\n" + tail;
	}

	std::cout << CLR_YELLOW "│ ⚡ " << toolName << " 返回 " << rawContent.size()
		<< " 字符，正在 AI 摘要…" CLR_RESET << std::endl;

	std::string content = utf8SafeSubstr(rawContent, MAX_PROMPT);

	std::string prompt = "请将以下 [" + toolName + "] 工具的返回结果整理为要点摘要，"
		"保留所有关键信息、数据、链接、文件名和事实，丢弃无信息量的格式噪声：\n\n" + content;

	json result = summarizeModel(prompt);
	if (!result.is_null() && result.contains("content") && result["content"].is_string()) {
		std::string summary = result["content"].get<std::string>();
		std::cout << CLR_DIM "│   摘要完成: " << rawContent.size()
			<< " → " << summary.size() << " 字符" CLR_RESET << std::endl;
		return "[AI摘要] " + summary;
	}

	std::cout << CLR_RED "│ ⚠ 摘要失败，回退截断" CLR_RESET << std::endl;
	return utf8SafeSubstr(rawContent, MIN_CHARS)
		+ "\n...[内容过长已截断，原长 " + std::to_string(rawContent.size()) + " 字符]";
}

json agent::truncateMsgContent(json msg, int maxChars) {
	if (msg.contains("content") && msg["content"].is_string()) {
		std::string c = msg["content"].get<std::string>();
		std::string safe = sanitizeUtf8Text(c);
		if ((int)safe.size() > maxChars) {
			msg["content"] = utf8SafeSubstr(safe, maxChars)
				+ "\n...[后续内容已截断，原长 " + std::to_string(safe.size()) + " 字符]";
		} else {
			msg["content"] = safe;
		}
	}
	return msg;
}

json agent::summarizeHistory(json& old_messages) {
	// 所有阈值基于主模型 context_window 自动推导（修改 model 配置无需手动调这里）
	const int CW = Config::get().main_model().context_window;
	const int SUM_CW = Config::get().summary_model().context_window;
	const int MAX_TOKENS      = int(CW * 0.90);  // 压缩触发：上下文 90%
	const int KEEP_RECENT      = 30;               // 固定保留最近 30 条
	const int MAX_PROMPT_CHARS = int(std::min(CW, SUM_CW) * 0.064); // 馈入摘要模型的上限
	const int MAX_MSG_CHARS    = int(CW * 0.008);  // 单条对话内容上限
	const int MAX_TOOL_KEPT    = int(CW * 0.064);  // 工具结果截断上限
	const int MAX_MSG_KEPT     = int(CW * 0.064);  // 普通消息截断上限

	int total = (int)old_messages.size();

	// 第一步：截断超长单条消息
	for (int i = 0; i < total; i++) {
		std::string role = old_messages[i].value("role", "");
		int cap = (role == "tool") ? MAX_TOOL_KEPT : MAX_MSG_KEPT;
		old_messages[i] = truncateMsgContent(old_messages[i], cap);
	}

	int total_tok = estimateTokens(old_messages);

	if (total_tok <= MAX_TOKENS) {
		return old_messages;
	}

	std::cout << CLR_YELLOW "│ ⚡ Token 超限(" << total_tok << "/" << MAX_TOKENS << ")，智能压缩中…" CLR_RESET << std::endl;

	json result = json::array();

	// 第二步：定位 system prompt（如果有）
	int sys_idx = -1;
	for (int i = 0; i < total; i++) {
		if (old_messages[i].value("role", "") == "system") {
			result.push_back(old_messages[i]);
			sys_idx = i;
			break;
		}
	}

	// 第三步：检测之前的历史摘要（逐轮累积的用户画像 + 事件记录）
	bool hasPrevSummary = false;
	for (int i = sys_idx + 1; i < total; i++) {
		auto& msg = old_messages[i];
		if (msg.value("role", "") == "user" && msg.contains("content") && msg["content"].is_string()) {
			std::string c = msg["content"].get<std::string>();
			if (c.find("[历史摘要") != std::string::npos || c.find("[长期记忆") != std::string::npos) {
				hasPrevSummary = true;
				break;
			}
		}
	}

	// 第四步：划分"待概括区"和"近期保留区"
	int keep_start = std::max(sys_idx + 1, total - KEEP_RECENT);

	// 第五步：构造智能压缩 prompt
	std::ostringstream prompt_builder;

	// ── 智能概括 prompt（结构化输出，要求丢弃细枝末节） ──
	prompt_builder << "你是一个AI记忆系统。请阅读以下对话历史（可能包含往轮摘要），"
		"提炼为一份**结构化长期记忆**。\n\n"
		"## 输出格式（严格按此结构，无内容的部分写\"无\"）\n\n"
		"### 用户画像\n"
		"- 身份/角色：（推断）\n"
		"- 技术栈/关注领域：\n"
		"- 偏好与习惯：\n\n"
		"### 重要人物与实体\n"
		"仅列出**反复出现或深入讨论**的人物/实体。仅提1-2次且无实质内容的**必须省略**。\n"
		"格式：- 名称：关系/角色 + 关键信息\n\n"
		"### 关键事件与操作时间线\n"
		"按时间顺序列出用户的核心请求、AI的工具调用及结果、重要产出。每次交互一行：\n"
		"- [用户请求] ...\n"
		"- [工具调用] xxx → 结果概要\n"
		"- [结论/产出] ...\n\n"
		"### 技术细节备忘\n"
		"保留重要代码片段、配置参数、文件路径、错误信息等。格式：\n"
		"- 简述：具体内容\n\n"
		"### 当前状态\n"
		"- 进行中的任务：\n"
		"- 待解决问题：\n"
		"- 下一步计划：\n\n"
		"## 压缩铁律\n"
		"- **丢弃**：日常寒暄、纯情绪表达、重复信息、无信息量的过渡语、仅提一两次的无关名字/实体\n"
		"- **保留**：用户核心意图、重大决策、工具操作结果、关键事实数据、文件路径\n"
		"- **合并**：相似话题多次讨论合并为一个要点\n"
		"- 输出使用中文，简洁精准，不要展开无关细节\n\n"
		"---\n"
		"## 原始对话历史\n\n";

	std::string prompt = prompt_builder.str();
	int chars_left = MAX_PROMPT_CHARS - (int)prompt.size();

	// 第六步：将待概括的消息序列化到 prompt
	for (int i = sys_idx + 1; i < keep_start && chars_left > 0; i++) {
		auto& msg = old_messages[i];
		std::string role = msg.value("role", "unknown");
		std::string line;

		// 跳过之前的历史摘要——单独标注，避免重复概括
		if (role == "user" && hasPrevSummary && msg.contains("content") && msg["content"].is_string()) {
			std::string c = msg["content"].get<std::string>();
			if (c.find("[历史摘要") != std::string::npos || c.find("[长期记忆") != std::string::npos) {
				line = "[上轮长期记忆]: " + c + "\n";
				if ((int)line.size() > chars_left) {
					line = utf8SafeSubstr(line, chars_left) + "\n";
					prompt += line;
					chars_left = 0;
					break;
				}
				prompt += line;
				chars_left -= (int)line.size();
				continue;
			}
		}

		if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
			// 工具调用消息：保留完整调用信息
			std::string tool_info;
			for (auto& tc : msg["tool_calls"]) {
				if (tc.contains("function") && tc["function"].contains("name")) {
					tool_info += "  [" + tc["function"]["name"].get<std::string>() + "]";
					if (tc["function"].contains("arguments")) {
						std::string args_str = tc["function"]["arguments"].is_string()
							? tc["function"]["arguments"].get<std::string>()
							: tc["function"]["arguments"].dump();
						if (args_str.size() > 200) args_str = utf8SafeSubstr(args_str, 200) + "...";
						tool_info += "(" + args_str + ")";
					}
				}
			}
			line = "[assistant 调用工具]:" + tool_info + "\n";
		}
		else if (msg.contains("content") && msg["content"].is_string()) {
			std::string content = msg["content"].get<std::string>();
			if (content.empty()) continue;

			if (role == "tool") {
				// 工具返回：保留首部（关键结果通常在开头）
				if (content.size() > 500) content = utf8SafeSubstr(content, 500) + "...";
				line = "[tool返回]: " + content + "\n";
			}
			else {
				if ((int)content.size() > MAX_MSG_CHARS) {
					content = utf8SafeSubstr(content, MAX_MSG_CHARS) + "...";
				}
				line = "[" + role + "]: " + content + "\n";
			}
		}
		else {
			continue;
		}

		if ((int)line.size() > chars_left) {
			line = utf8SafeSubstr(line, chars_left) + "\n";
			prompt += line;
			chars_left = 0;
			break;
		}
		prompt += line;
		chars_left -= (int)line.size();
	}

	// 第七步：调用概括模型生成结构化长期记忆
	json summary = summarizeModel(prompt);
	if (!summary.is_null() && summary.contains("content") && summary["content"].is_string()) {
		json summary_msg;
		summary_msg["role"] = "user";
		// 用 "[长期记忆]" 标记，方便后续轮次识别和合并
		summary_msg["content"] = "[长期记忆] 以下为此前对话的结构化记忆，优先参考其中信息：\n"
			+ summary["content"].get<std::string>();
		result.push_back(summary_msg);
		std::cout << CLR_DIM "│   智能摘要(" << summary["content"].get<std::string>().size() << " 字符)" CLR_RESET << std::endl;
	}
	else {
		std::cout << CLR_RED "│ ⚠ 摘要失败，仅保留最近消息" CLR_RESET << std::endl;
	}

	// 第八步：保留最近 KEEP_RECENT 条消息（保持当前对话连贯）
	for (int i = keep_start; i < total; i++) {
		if (old_messages[i].value("role", "") != "system") {
			result.push_back(old_messages[i]);
		}
	}

	std::cout << CLR_DIM "│   压缩: " << old_messages.size() << " → " << result.size()
		<< " 条消息 (~" << estimateTokens(result) << " tokens)" CLR_RESET << std::endl;
	return result;
}

// Phase 2: Reasoning Engine - 多轮推理循环
std::string agent::run_loop(const std::string& initial_message) {
	_is_reasoning.store(true);
	{
		std::lock_guard<std::mutex> lock(_turnQueueMutex);
		_turn_queue.clear();
	}

	std::string current_input = initial_message;
	std::string final_reply;
	int turn_count = 0;

	while (turn_count < _max_reasoning_turns) {
		turn_count++;

		// 处理当前消息
		json userMsg;
		userMsg["role"] = "user";
		userMsg["content"] = current_input;
		messages.push_back(userMsg);

		// 调用 processQuery 执行工具循环
		std::string reply = processQuery(current_input, current_input);

		if (!reply.empty()) {
			final_reply = reply;
		}

		// 检查队列是否有新消息
		TurnMessage next;
		bool hasNext = false;
		{
			std::lock_guard<std::mutex> lock(_turnQueueMutex);
			if (!_turn_queue.empty()) {
				next = _turn_queue.front();
				_turn_queue.erase(_turn_queue.begin());
				hasNext = true;
			}
		}
		if (!hasNext) {
			break;
		}

		current_input = next.content;

		if (!next.sender_id.empty()) {
			current_input = "[" + next.sender_id + "]: " + current_input;
		}
	}

	_is_reasoning.store(false);

	// 回复质量优化
	if (!final_reply.empty()) {
		auto& optimizer = ReplyOptimizer::get();

		// 优化回复长度（基于群风格）
		final_reply = optimizer.optimizeLength(final_reply, _groupStyle, 200);

		// 调整语气（基于群风格）
		final_reply = optimizer.adjustTone(final_reply, _groupStyle);

		// 过滤敏感内容
		final_reply = optimizer.filterSensitive(final_reply);

		// 添加表情（基于群风格偏好）
		if (_groupStyle.find("表情包偏好") != std::string::npos) {
			final_reply = optimizer.addEmoji(final_reply, _groupStyle);
		}
	}

	return final_reply;
}

void agent::injectTurnMessage(const std::string& content, const std::string& sender_id) {
	if (!_is_reasoning) return;

	TurnMessage msg;
	msg.content = content;
	msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	msg.sender_id = sender_id;

	std::lock_guard<std::mutex> lock(_turnQueueMutex);
	_turn_queue.push_back(msg);
}
