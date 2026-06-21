#include "main_system.h"
#include "src/infra/logger.h"
#include "src/core/config.h"
#include "src/plugin/persistence.h"
#include <thread>
#include <chrono>
#include <signal.h>

// 全局指针用于信号处理
static MainSystem* g_main_system = nullptr;

// 信号处理函数
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        Logger::get().info("[系统]", "收到终止信号");
        if (g_main_system) {
            g_main_system->shutdown();
        }
    }
}

// ──────────────────────────────────────────────────────
// MainSystem 实现
// ──────────────────────────────────────────────────────

MainSystem& MainSystem::get() {
    static MainSystem instance;
    return instance;
}

MainSystem::MainSystem()
    : _running(false),
      _initialized(false),
      _process_type(ProcessType::WORKER) {
    g_main_system = this;
}

bool MainSystem::initialize() {
    try {
        Logger::get().info("[系统]", "========== 系统初始化开始 ==========");

        // 0. 初始化持久化系统
        auto& persistence = PersistenceManager::get();
        persistence.initialize("./data");
        Logger::get().info("[系统]", "持久化系统初始化完成");

        // 1. 初始化配置系统
        if (!init_config()) {
            Logger::get().error("[系统]", "配置系统初始化失败");
            return false;
        }

        // 2. 初始化进程管理系统
        if (!init_process_manager()) {
            Logger::get().error("[系统]", "进程管理系统初始化失败");
            return false;
        }

        // 3. 初始化插件系统
        if (!init_plugins()) {
            Logger::get().error("[系统]", "插件系统初始化失败");
            return false;
        }

        _initialized = true;

        Logger::get().info("[系统]", "========== 系统初始化完成 ==========");
        Logger::get().info("[系统]", "进程类型: " + std::string(_process_type == ProcessType::RUNNER ? "Runner" : "Worker"));

        return true;

    } catch (const std::exception& e) {
        Logger::get().error("[系统]", "初始化异常: " + std::string(e.what()));
        return false;
    }
}

bool MainSystem::init_config() {
    try {
        Logger::get().info("[系统]", "初始化配置系统...");
        Config::get().load("config.json");
        Logger::get().info("[系统]", "配置加载完成");
        return true;
    } catch (const std::exception& e) {
        Logger::get().error("[系统]", "配置加载失败: " + std::string(e.what()));
        return false;
    }
}

bool MainSystem::init_process_manager() {
    try {
        Logger::get().info("[系统]", "初始化进程管理器...");

        // 从环境变量检查进程类型
        const char* worker_env = std::getenv("MAIBOT_WORKER_PROCESS");
        if (worker_env && std::string(worker_env) == "1") {
            _process_type = ProcessType::WORKER;
        } else {
            _process_type = ProcessType::RUNNER;
        }

        auto& pm = ProcessManager::get();
        pm.initialize(_process_type);
        pm.set_restart_delay(1);
        pm.set_max_restarts(5);

        Logger::get().info("[系统]", "进程管理器初始化完成");
        return true;

    } catch (const std::exception& e) {
        Logger::get().error("[系统]", "进程管理器初始化失败: " + std::string(e.what()));
        return false;
    }
}

bool MainSystem::init_plugins() {
    try {
        Logger::get().info("[系统]", "初始化插件系统...");

        // 加载所有内置插件
        auto& loader = BuiltinPluginLoader::get();
        loader.load_all_plugins();

        auto plugins = loader.get_loaded_plugins();
        Logger::get().info("[系统]", "已加载 " + std::to_string(plugins.size()) + " 个插件");

        for (const auto& plugin_name : plugins) {
            Logger::get().debug("[系统]", "- " + plugin_name);
        }

        Logger::get().info("[系统]", "插件系统初始化完成");
        return true;

    } catch (const std::exception& e) {
        Logger::get().error("[系统]", "插件系统初始化失败: " + std::string(e.what()));
        return false;
    }
}

void MainSystem::run() {
    if (!_initialized) {
        Logger::get().error("[系统]", "系统未初始化，无法运行");
        return;
    }

    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    _running = true;
    Logger::get().info("[系统]", "系统开始运行");

    main_loop();
}

void MainSystem::main_loop() {
    int loop_count = 0;

    while (_running) {
        try {
            loop_count++;

            // 每 60 秒打印一次系统状态
            if (loop_count % 60 == 0) {
                auto& pm = ProcessManager::get();
                auto health = pm.check_health();

                Logger::get().info("[系统]", "心跳 - 状态: " + health.status +
                                  ", 运行时间: " + std::to_string(health.uptime_seconds) + "s");

                // 打印插件列表
                auto& plg_manager = PluginManager::get();
                auto tools = plg_manager.list_tools();
                Logger::get().debug("[系统]", "已注册 " + std::to_string(tools.size()) + " 个工具");
            }

            // 每 5 秒进行一次健康检查
            if (loop_count % 5 == 0) {
                auto& pm = ProcessManager::get();
                auto health = pm.check_health();

                if (!health.is_healthy) {
                    Logger::get().warn("[系统]", "系统健康状态异常: " + health.status);
                }
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));

        } catch (const std::exception& e) {
            Logger::get().error("[系统]", "主循环异常: " + std::string(e.what()));
        }
    }

    Logger::get().info("[系统]", "系统主循环已退出");
}

void MainSystem::shutdown() {
    if (!_running) {
        return;
    }

    Logger::get().info("[系统]", "========== 系统关闭开始 ==========");

    _running = false;

    // 卸载所有插件
    auto& manager = PluginManager::get();
    auto plugins = BuiltinPluginLoader::get().get_loaded_plugins();

    for (const auto& plugin_name : plugins) {
        Logger::get().info("[系统]", "卸载插件: " + plugin_name);
        manager.unload_plugin(plugin_name);
        manager.unregister_plugin(plugin_name);
    }

    // 关闭进程管理器
    auto& pm = ProcessManager::get();
    pm.shutdown();

    Logger::get().info("[系统]", "========== 系统关闭完成 ==========");
    Logger::get().info("[系统]", "系统已停止运行");
}


