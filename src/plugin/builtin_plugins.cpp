#include "src/plugin/builtin_plugins.h"
#include "src/infra/logger.h"

BuiltinPluginLoader& BuiltinPluginLoader::get() {
    static BuiltinPluginLoader instance;
    return instance;
}

void BuiltinPluginLoader::load_all_plugins() {
    Logger::get().info("[插件加载器]", "开始加载所有内置插件");

    load_embedding_plugin();
    load_knowledge_base_plugin();
    load_command_handler_plugin();
    load_long_term_memory_plugin();
    load_user_management_plugin();

    Logger::get().info("[插件加载器]", "已加载 " + std::to_string(_loaded_plugins.size()) + " 个插件");
}

void BuiltinPluginLoader::load_embedding_plugin() {
    try {
        Logger::get().info("[插件加载器]", "加载向量系统插件...");

        auto& manager = PluginManager::get();

        // 创建插件实例
        auto plugin = create_embedding_system_plugin();

        // 注册到管理器
        manager.register_plugin("embedding_system", plugin);

        // 加载插件
        if (manager.load_plugin("embedding_system")) {
            _loaded_plugins.push_back("embedding_system");
            Logger::get().info("[插件加载器]", "向量系统插件加载成功");
        } else {
            Logger::get().error("[插件加载器]", "向量系统插件加载失败");
        }

    } catch (const std::exception& e) {
        Logger::get().error("[插件加载器]", "加载向量系统插件时出错: " + std::string(e.what()));
    }
}

std::vector<std::string> BuiltinPluginLoader::get_loaded_plugins() const {
    return _loaded_plugins;
}

void BuiltinPluginLoader::load_knowledge_base_plugin() {
    try {
        Logger::get().info("[插件加载器]", "加载知识库插件...");

        auto& manager = PluginManager::get();

        // 创建插件实例
        auto plugin = create_knowledge_base_plugin();

        // 注册到管理器
        manager.register_plugin("knowledge_base", plugin);

        // 加载插件
        if (manager.load_plugin("knowledge_base")) {
            _loaded_plugins.push_back("knowledge_base");
            Logger::get().info("[插件加载器]", "知识库插件加载成功");
        } else {
            Logger::get().error("[插件加载器]", "知识库插件加载失败");
        }

    } catch (const std::exception& e) {
        Logger::get().error("[插件加载器]", "加载知识库插件时出错: " + std::string(e.what()));
    }
}

void BuiltinPluginLoader::load_command_handler_plugin() {
    try {
        Logger::get().info("[插件加载器]", "加载命令处理插件...");

        auto& manager = PluginManager::get();

        // 创建插件实例
        auto plugin = create_command_handler_plugin();

        // 注册到管理器
        manager.register_plugin("command_handler", plugin);

        // 加载插件
        if (manager.load_plugin("command_handler")) {
            _loaded_plugins.push_back("command_handler");
            Logger::get().info("[插件加载器]", "命令处理插件加载成功");
        } else {
            Logger::get().error("[插件加载器]", "命令处理插件加载失败");
        }

    } catch (const std::exception& e) {
        Logger::get().error("[插件加载器]", "加载命令处理插件时出错: " + std::string(e.what()));
    }
}

void BuiltinPluginLoader::load_long_term_memory_plugin() {
    try {
        Logger::get().info("[插件加载器]", "加载长期记忆插件...");

        auto& manager = PluginManager::get();

        // 创建插件实例
        auto plugin = create_long_term_memory_plugin();

        // 注册到管理器
        manager.register_plugin("long_term_memory", plugin);

        // 加载插件
        if (manager.load_plugin("long_term_memory")) {
            _loaded_plugins.push_back("long_term_memory");
            Logger::get().info("[插件加载器]", "长期记忆插件加载成功");
        } else {
            Logger::get().error("[插件加载器]", "长期记忆插件加载失败");
        }

    } catch (const std::exception& e) {
        Logger::get().error("[插件加载器]", "加载长期记忆插件时出错: " + std::string(e.what()));
    }
}

void BuiltinPluginLoader::load_user_management_plugin() {
    try {
        Logger::get().info("[插件加载器]", "加载用户管理插件...");

        auto& manager = PluginManager::get();

        // 创建插件实例
        auto plugin = create_user_management_plugin();

        // 注册到管理器
        manager.register_plugin("user_management", plugin);

        // 加载插件
        if (manager.load_plugin("user_management")) {
            _loaded_plugins.push_back("user_management");
            Logger::get().info("[插件加载器]", "用户管理插件加载成功");
        } else {
            Logger::get().error("[插件加载器]", "用户管理插件加载失败");
        }

    } catch (const std::exception& e) {
        Logger::get().error("[插件加载器]", "加载用户管理插件时出错: " + std::string(e.what()));
    }
}


