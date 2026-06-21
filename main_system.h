#pragma once

#include "src/core/base.h"
#include "src/plugin/plugin_base.h"
#include "src/plugin/process_manager.h"
#include "src/plugin/builtin_plugins.h"
#include "src/plugin/persistence.h"
#include <memory>

// ──────────────────────────────────────────────────────
// 主系统类 - 集成 P0/P1/P2
// ──────────────────────────────────────────────────────

class MainSystem {
public:
    static MainSystem& get();

    // 初始化系统
    bool initialize();

    // 运行系统主循环
    void run();

    // 优雅关闭
    void shutdown();

    // 获取系统状态
    bool is_running() const { return _running; }

    // 获取插件管理器（供外部使用）
    PluginManager& get_plugin_manager() { return PluginManager::get(); }

    // 获取进程管理器（供外部使用）
    ProcessManager& get_process_manager() { return ProcessManager::get(); }

private:
    MainSystem();

    // 初始化步骤
    bool init_config();
    bool init_plugins();
    bool init_process_manager();

    // 主循环
    void main_loop();

    // 状态
    bool _running;
    bool _initialized;
    ProcessType _process_type;
};
