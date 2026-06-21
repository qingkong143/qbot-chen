#include "src/infra/logger.h"

Logger& Logger::get() {
    static Logger instance;
    return instance;
}

std::string Logger::formatTime() const {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local;
#ifdef _WIN32
    localtime_s(&local, &tt);
#else
    localtime_r(&tt, &local);
#endif

    std::ostringstream oss;
    oss << std::put_time(&local, "%H:%M:%S");
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) const {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "UNKNOWN";
    }
}

void Logger::output(LogLevel level, const std::string& tag, const std::string& message) {
    if (level < _level) {
        return;  // 日志级别低于阈值，不输出
    }

    std::string timestamp = formatTime();
    std::string levelStr = levelToString(level);
    std::string output = "[" + timestamp + "] [" + levelStr + "] [" + tag + "] " + message;

    // 错误级别输出到 stderr，其他到 stdout
    if (level == LogLevel::Error) {
        std::cerr << output << std::endl;
    } else {
        std::cout << output << std::endl;
    }
}

void Logger::debug(const std::string& tag, const std::string& message) {
    output(LogLevel::Debug, tag, message);
}

void Logger::info(const std::string& tag, const std::string& message) {
    output(LogLevel::Info, tag, message);
}

void Logger::warn(const std::string& tag, const std::string& message) {
    output(LogLevel::Warn, tag, message);
}

void Logger::error(const std::string& tag, const std::string& message) {
    output(LogLevel::Error, tag, message);
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& message) {
    output(level, tag, message);
}
