#pragma once

#include "src/core/base.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <cstdint>

// ──────────────────────────────────────────────────────
// 进程管理模型
// ──────────────────────────────────────────────────────

enum class ProcessType {
    RUNNER,   // 守护进程
    WORKER    // 工作进程
};

enum class ProcessState {
    STARTING,
    RUNNING,
    PAUSED,
    STOPPING,
    STOPPED,
    CRASHED
};

// 进程健康检查结果
struct HealthCheckResult {
    bool is_healthy;
    std::string status;
    int64_t uptime_seconds;
    int error_count;
    std::string last_error;
};

// 进程信息
struct ProcessInfo {
    ProcessType type;
    ProcessState state;
    uint32_t pid;
    int64_t start_time;
    int64_t last_heartbeat;
    int restart_count;
};

// ──────────────────────────────────────────────────────
// 工作进程管理
// ──────────────────────────────────────────────────────

class WorkerProcess {
public:
    static WorkerProcess& get();

    // 初始化
    void initialize();

    // 主循环
    void run();

    // 状态查询
    ProcessInfo get_process_info() const;
    HealthCheckResult check_health();

    // 心跳信号
    void send_heartbeat();

    // 优雅关闭
    void shutdown();

    // 检查是否应该重启
    bool should_restart() const;

private:
    WorkerProcess();

    ProcessState _state;
    int64_t _start_time;
    int64_t _last_heartbeat;
    int _restart_count;
    int _error_count;
    std::string _last_error;

    std::atomic<bool> _running{false};
    mutable std::mutex _state_mutex;
};

// ──────────────────────────────────────────────────────
// 守护进程管理
// ──────────────────────────────────────────────────────

class RunnerProcess {
public:
    static RunnerProcess& get();

    // 启动 Runner
    void start();

    // 获取被监控的 Worker PID
    uint32_t get_worker_pid() const;

    // 是否应该继续运行
    bool is_running() const;

    // 优雅关闭
    void shutdown();

private:
    RunnerProcess();

    // 启动 Worker 进程
    uint32_t spawn_worker();

    // 监控 Worker 进程
    void monitor_worker();

    // 处理 Worker 崩溃
    void handle_worker_crash(int exit_code);

    // 重启 Worker
    void restart_worker();

    ProcessInfo _worker_info;
    std::atomic<bool> _running{false};
    mutable std::mutex _worker_mutex;
};

// ──────────────────────────────────────────────────────
// 进程管理器
// ──────────────────────────────────────────────────────

class ProcessManager {
public:
    static ProcessManager& get();

    // 初始化进程管理
    void initialize(ProcessType type);

    // 获取当前进程类型
    ProcessType get_process_type() const { return _process_type; }

    // 获取进程信息
    ProcessInfo get_info() const;

    // 健康检查
    HealthCheckResult check_health();

    // 优雅关闭
    void shutdown();

    // 获取重启次数
    int get_restart_count() const { return _restart_count; }

    // 设置重启延迟（秒）
    void set_restart_delay(int seconds) { _restart_delay_seconds = seconds; }

    // 设置最大重启次数
    void set_max_restarts(int max) { _max_restarts = max; }

private:
    ProcessManager();

    ProcessType _process_type;
    ProcessState _state;
    int _restart_count;
    int _restart_delay_seconds;
    int _max_restarts;
    int64_t _start_time;
    mutable std::mutex _state_mutex;
};

// ──────────────────────────────────────────────────────
// 退出码定义
// ──────────────────────────────────────────────────────

constexpr int PROCESS_EXIT_SUCCESS = 0;
constexpr int PROCESS_EXIT_RESTART = 42;  // MaiBot 风格的重启退出码
constexpr int PROCESS_EXIT_ERROR = 1;
constexpr int PROCESS_EXIT_CTRL_C = 130;
