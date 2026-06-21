#include "src/core/command.h"
#include "src/core/config.h"
#include <chrono>
#include <ctime>
#include <sstream>

using json = nlohmann::json;

void CommandHandler::registerCommand(const CommandEntry& cmd) {
    _commands[cmd.name] = cmd;
}

const CommandEntry* CommandHandler::match(
    const std::string& text, std::string& args_out) const
{
    for (auto& [name, cmd] : _commands) {
        if (cmd.prefix.empty()) {
            // 无前缀命令（如 "赞我"）：精确匹配
            if (text == cmd.name) {
                args_out.clear();
                return &cmd;
            }
        } else {
            // 有前缀命令：text 以 prefix+name 开头
            std::string prefixName = cmd.prefix + cmd.name;
            if (text.size() >= prefixName.size() &&
                text.substr(0, prefixName.size()) == prefixName) {
                args_out = (text.size() > prefixName.size())
                    ? text.substr(prefixName.size()) : "";
                // 跳过前缀后的首个空白符
                size_t start = args_out.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    args_out = args_out.substr(start);
                } else {
                    args_out.clear();
                }
                return &cmd;
            }
        }
    }
    return nullptr;
}

std::string CommandHandler::execute(
    const CommandEntry& cmd, const std::string& args) const
{
    try {
        json jargs = json::parse(args);
        return cmd.executor(jargs);
    } catch (...) {
        // 空参数时传空对象
        try {
            return cmd.executor(json::object());
        } catch (...) {
            return "[命令执行出错]";
        }
    }
}

void CommandHandler::registerBuiltins() {
    // /ping — 测试在线
    registerCommand({
        "/", "ping", "测试机器人是否在线",
        [](const json&) -> std::string {
            return "这是一个由chenshuzhe143开发的C++机器人";
        },
        CommandMatchLevel::Intercept
    });

    // /time — 当前时间
    registerCommand({
        "/", "time", "显示当前时间",
        [](const json&) -> std::string {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char time_buf[64] = {};
#ifdef _WIN32
            ctime_s(time_buf, sizeof(time_buf), &t);
#else
            ctime_r(&t, time_buf);
#endif
            std::string time_str = time_buf;
            if (!time_str.empty() && time_str.back() == '\n') time_str.pop_back();
            return "当前时间: " + time_str;
        },
        CommandMatchLevel::Intercept
    });

    // 赞我 — 精准匹配
    registerCommand({
        "", "赞我", "给用户点赞",
        [](const json&) -> std::string {
            return "已经给你点了10个大大的赞呢~";
        },
        CommandMatchLevel::Intercept
    });
}
