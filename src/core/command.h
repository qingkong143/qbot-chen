#pragma once
#include "src/core/base.h"
#include <functional>
#include <optional>

using json = nlohmann::json;

// 命令匹配结果
enum class CommandMatchLevel {
    None,       // 不匹配
    Intercept,  // 命中且拦截（不走 HeartFlow）
    Continue    // 命中但继续走主链（类似 HeartFlow）
};

// 命令定义
struct CommandEntry {
    std::string prefix;       // 前缀（如 "/" 或空字符串）
    std::string name;         // 命令名
    std::string description;  // 描述
    std::function<std::string(const json& args)> executor; // 执行器
    CommandMatchLevel level = CommandMatchLevel::Intercept; // 默认拦截
};

// 命令处理器
class CommandHandler {
public:
    // 注册命令
    void registerCommand(const CommandEntry& cmd);

    // 匹配消息是否为命令；返回 (匹配到的CommandEntry, 参数)
    // 未匹配时返回 nullptr
    const CommandEntry* match(const std::string& text, std::string& args_out) const;

    // 执行匹配到的命令
    std::string execute(const CommandEntry& cmd, const std::string& args) const;

    // 内置命令注册
    void registerBuiltins();

private:
    // 命令名 → 命令条目
    std::unordered_map<std::string, CommandEntry> _commands;
};
