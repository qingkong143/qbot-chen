#include "src/plugin/process_manager.h"
#include "src/infra/logger.h"
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>

#ifdef _WIN32
    #include <windows.h>
    #include <processthreadsapi.h>
#else
    #include <unistd.h>
    #include <sys/wait.h>
    #include <signal.h>
#endif

// ──────────────────────────────────────────────────────
// WorkerProcess 实现
// ──────────────────────────────────────────────────────

WorkerProcess& WorkerProcess::get() {
    static WorkerProcess instance;
    return instance;
}

WorkerProcess::WorkerProcess()
    : _state(ProcessState::STOPPED),
      _start_time(0),
      _last_heartbeat(0),
      _restart_count(0),
      _error_count(0) {}

void WorkerProcess::initialize() {
    std::lock_guard<std::mutex> lock(_state_mutex);
    _state = ProcessState::STARTING;
    _start_time = std::time(nullptr);
    _last_heartbeat = _start_time;
    Logger::get().info("[Worker]", "进程初始化完成");
}

void WorkerProcess::run() {
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _state = ProcessState::RUNNING;
        _running = true;
    }

    Logger::get().info("[Worker]", "进程开始运行");

    // 主循环 - 这里是 Worker 的实际工作
    while (_running) {
        try {
            send_heartbeat();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        } catch (const std::exception& e) {
            Logger::get().error("[Worker]", "错误: " + std::string(e.what()));
            _error_count++;
            _last_error = e.what();
        }
    }

    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _state = ProcessState::STOPPED;
    }

    Logger::get().info("[Worker]", "进程已停止");
}

ProcessInfo WorkerProcess::get_process_info() const {
    std::lock_guard<std::mutex> lock(_state_mutex);

#ifdef _WIN32
    uint32_t pid = GetCurrentProcessId();
#else
    uint32_t pid = getpid();
#endif

    return ProcessInfo{
        ProcessType::WORKER,
        _state,
        pid,
        _start_time,
        _last_heartbeat,
        _restart_count
    };
}

HealthCheckResult WorkerProcess::check_health() {
    std::lock_guard<std::mutex> lock(_state_mutex);

    int64_t now = std::time(nullptr);
    int64_t uptime = now - _start_time;

    bool is_healthy = _state == ProcessState::RUNNING &&
                      _error_count < 10 &&
                      (now - _last_heartbeat) < 30;  // 30秒内有心跳

    std::string status;
    switch (_state) {
        case ProcessState::RUNNING:
            status = "运行中";
            break;
        case ProcessState::PAUSED:
            status = "暂停";
            break;
        case ProcessState::STOPPED:
            status = "已停止";
            break;
        case ProcessState::CRASHED:
            status = "已崩溃";
            break;
        case ProcessState::STARTING:
            status = "启动中";
            break;
        case ProcessState::STOPPING:
            status = "停止中";
            break;
    }

    return HealthCheckResult{
        is_healthy,
        status,
        uptime,
        _error_count,
        _last_error
    };
}

void WorkerProcess::send_heartbeat() {
    std::lock_guard<std::mutex> lock(_state_mutex);
    _last_heartbeat = std::time(nullptr);
}

void WorkerProcess::shutdown() {
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (_state == ProcessState::STOPPED) {
            return;
        }
        _state = ProcessState::STOPPING;
    }

    _running = false;
    Logger::get().info("[Worker]", "收到关闭信号");

    // 等待最多 5 秒完成清理
    for (int i = 0; i < 50; ++i) {
        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            if (_state == ProcessState::STOPPED) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool WorkerProcess::should_restart() const {
    std::lock_guard<std::mutex> lock(_state_mutex);
    return _state == ProcessState::CRASHED && _restart_count < 5;
}

// ──────────────────────────────────────────────────────
// RunnerProcess 实现
// ──────────────────────────────────────────────────────

RunnerProcess& RunnerProcess::get() {
    static RunnerProcess instance;
    return instance;
}

RunnerProcess::RunnerProcess() {
    _worker_info.type = ProcessType::WORKER;
    _worker_info.state = ProcessState::STOPPED;
    _worker_info.pid = 0;
    _worker_info.restart_count = 0;
}

void RunnerProcess::start() {
    _running = true;
    Logger::get().info("[Runner]", "守护进程启动");

    // 启动 Worker
    _worker_info.pid = spawn_worker();
    if (_worker_info.pid == 0) {
        Logger::get().error("[Runner]", "无法启动 Worker 进程");
        _running = false;
        return;
    }

    Logger::get().info("[Runner]", "Worker 进程已启动, PID=" + std::to_string(_worker_info.pid));

    // 监控 Worker
    monitor_worker();
}

uint32_t RunnerProcess::spawn_worker() {
    // 这是一个简化的实现
    // 实际场景中需要用 fork/CreateProcess 启动子进程
    Logger::get().info("[Runner]", "生成 Worker 进程");

#ifdef _WIN32
    // Windows 下的进程启动逻辑
    return GetCurrentProcessId();  // 简化实现
#else
    // Linux 下的进程启动逻辑
    return getpid();  // 简化实现
#endif
}

void RunnerProcess::monitor_worker() {
    Logger::get().info("[Runner]", "开始监控 Worker 进程");

    while (_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // 这里应该实现实际的进程监控逻辑
        // 检查 Worker 是否仍在运行
        // 如果崩溃，触发重启
    }
}

void RunnerProcess::handle_worker_crash(int exit_code) {
    Logger::get().error("[Runner]", "Worker 进程崩溃, 退出码=" + std::to_string(exit_code));

    if (exit_code == PROCESS_EXIT_RESTART) {
        Logger::get().info("[Runner]", "Worker 请求重启");
        restart_worker();
    } else if (_worker_info.restart_count < 5) {
        Logger::get().warn("[Runner]", "尝试重启 Worker");
        restart_worker();
    } else {
        Logger::get().error("[Runner]", "超过最大重启次数，放弃重启");
        _running = false;
    }
}

void RunnerProcess::restart_worker() {
    _worker_info.restart_count++;
    Logger::get().info("[Runner]", "重启 Worker (次数=" + std::to_string(_worker_info.restart_count) + ")");

    std::this_thread::sleep_for(std::chrono::seconds(1));  // 等待 1 秒后重启

    _worker_info.pid = spawn_worker();
}

uint32_t RunnerProcess::get_worker_pid() const {
    std::lock_guard<std::mutex> lock(_worker_mutex);
    return _worker_info.pid;
}

bool RunnerProcess::is_running() const {
    return _running;
}

void RunnerProcess::shutdown() {
    Logger::get().info("[Runner]", "收到关闭信号");
    _running = false;
}

// ──────────────────────────────────────────────────────
// ProcessManager 实现
// ──────────────────────────────────────────────────────

ProcessManager& ProcessManager::get() {
    static ProcessManager instance;
    return instance;
}

ProcessManager::ProcessManager()
    : _process_type(ProcessType::WORKER),
      _state(ProcessState::STOPPED),
      _restart_count(0),
      _restart_delay_seconds(1),
      _max_restarts(5),
      _start_time(0) {}

void ProcessManager::initialize(ProcessType type) {
    std::lock_guard<std::mutex> lock(_state_mutex);

    _process_type = type;
    _state = ProcessState::STARTING;
    _start_time = std::time(nullptr);

    Logger::get().info("[进程管理]", "初始化: " + std::string(type == ProcessType::RUNNER ? "Runner" : "Worker"));
}

ProcessInfo ProcessManager::get_info() const {
    std::lock_guard<std::mutex> lock(_state_mutex);

#ifdef _WIN32
    uint32_t pid = GetCurrentProcessId();
#else
    uint32_t pid = getpid();
#endif

    return ProcessInfo{
        _process_type,
        _state,
        pid,
        _start_time,
        std::time(nullptr),
        _restart_count
    };
}

HealthCheckResult ProcessManager::check_health() {
    std::lock_guard<std::mutex> lock(_state_mutex);

    int64_t now = std::time(nullptr);
    int64_t uptime = now - _start_time;

    bool is_healthy = _state == ProcessState::RUNNING && _restart_count < _max_restarts;

    std::string status;
    switch (_state) {
        case ProcessState::RUNNING:
            status = "运行中";
            break;
        case ProcessState::PAUSED:
            status = "暂停";
            break;
        case ProcessState::STOPPED:
            status = "已停止";
            break;
        case ProcessState::CRASHED:
            status = "已崩溃";
            break;
        case ProcessState::STARTING:
            status = "启动中";
            break;
        case ProcessState::STOPPING:
            status = "停止中";
            break;
    }

    return HealthCheckResult{
        is_healthy,
        status,
        uptime,
        0,
        ""
    };
}

void ProcessManager::shutdown() {
    std::lock_guard<std::mutex> lock(_state_mutex);
    _state = ProcessState::STOPPING;
    Logger::get().info("[进程管理]", "关闭中...");
}


