#include "src/plugin/plugin_loader.h"
#include "src/core/config.h"
#include <iostream>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

PluginLoader& PluginLoader::get() {
    static PluginLoader instance;
    return instance;
}

void* PluginLoader::loadLibrary(const std::string& path) {
#ifdef _WIN32
    return LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
}

void PluginLoader::unloadLibrary(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

void* PluginLoader::getSymbol(void* handle, const std::string& symbol) {
    if (!handle) return nullptr;
#ifdef _WIN32
    return GetProcAddress((HMODULE)handle, symbol.c_str());
#else
    return dlsym(handle, symbol.c_str());
#endif
}

bool PluginLoader::loadPlugin(const std::string& pluginPath) {
    std::lock_guard<std::mutex> lock(_mutex);

    // 1. 加载动态库
    void* handle = loadLibrary(pluginPath);
    if (!handle) {
        std::cerr << "[插件加载器] 加载失败: " << pluginPath << std::endl;
        return false;
    }

    // 2. 获取创建函数指针
    typedef IPlugin* (*CreatePluginFunc)();
    CreatePluginFunc createFunc = (CreatePluginFunc)getSymbol(handle, "createPlugin");
    if (!createFunc) {
        std::cerr << "[插件加载器] 无法找到 createPlugin 符号: " << pluginPath << std::endl;
        unloadLibrary(handle);
        return false;
    }

    // 3. 创建插件实例
    IPlugin* plugin = createFunc();
    if (!plugin) {
        std::cerr << "[插件加载器] 创建插件实例失败: " << pluginPath << std::endl;
        unloadLibrary(handle);
        return false;
    }

    // 4. 初始化插件
    if (!plugin->init()) {
        std::cerr << "[插件加载器] 插件初始化失败: " << plugin->getName() << std::endl;
        typedef void (*DestroyPluginFunc)(IPlugin*);
        DestroyPluginFunc destroyFunc = (DestroyPluginFunc)getSymbol(handle, "destroyPlugin");
        if (destroyFunc) destroyFunc(plugin);
        unloadLibrary(handle);
        return false;
    }

    // 5. 注册事件处理器
    EventBus::get().subscribeAsync(EventType::ON_MESSAGE, plugin->getName(),
        [plugin](const Event& e) {
            // 这里可以调用插件的事件处理逻辑
            return true;
        });

    // 6. 保存插件信息
    PluginInfo info;
    info.name = plugin->getName();
    info.path = pluginPath;
    info.version = plugin->getVersion();
    info.enabled = true;
    info.handle = handle;
    info.instance = plugin;

    _plugins[info.name] = info;
    std::cout << "[插件加载器] 加载成功: " << info.name << " v" << info.version << std::endl;

    return true;
}

bool PluginLoader::unloadPlugin(const std::string& pluginName) {
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = _plugins.find(pluginName);
    if (it == _plugins.end()) {
        return false;
    }

    PluginInfo& info = it->second;

    // 销毁插件
    if (info.instance) {
        info.instance->destroy();

        typedef void (*DestroyPluginFunc)(IPlugin*);
        DestroyPluginFunc destroyFunc = (DestroyPluginFunc)getSymbol(info.handle, "destroyPlugin");
        if (destroyFunc) destroyFunc(info.instance);
    }

    // 卸载库
    if (info.handle) {
        unloadLibrary(info.handle);
    }

    // 移除处理器
    EventBus::get().unsubscribe(EventType::ON_MESSAGE, pluginName);

    _plugins.erase(it);
    std::cout << "[插件加载器] 卸载成功: " << pluginName << std::endl;

    return true;
}

void PluginLoader::unloadAll() {
    std::lock_guard<std::mutex> lock(_mutex);

    for (auto it = _plugins.begin(); it != _plugins.end(); ++it) {
        PluginInfo& info = it->second;

        if (info.instance) {
            info.instance->destroy();

            typedef void (*DestroyPluginFunc)(IPlugin*);
            DestroyPluginFunc destroyFunc = (DestroyPluginFunc)getSymbol(info.handle, "destroyPlugin");
            if (destroyFunc) destroyFunc(info.instance);
        }

        if (info.handle) {
            unloadLibrary(info.handle);
        }
    }

    _plugins.clear();
}

std::vector<PluginInfo> PluginLoader::getLoadedPlugins() const {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<PluginInfo> result;
    for (const auto& [name, info] : _plugins) {
        result.push_back(info);
    }
    return result;
}

IPlugin* PluginLoader::getPlugin(const std::string& pluginName) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _plugins.find(pluginName);
    if (it != _plugins.end()) {
        return it->second.instance;
    }
    return nullptr;
}

bool PluginLoader::loadPluginsFromConfig(const std::string& configPath) {
    std::ifstream file(configPath);
    if (!file.good()) {
        std::cerr << "[插件加载器] 配置文件不存在: " << configPath << std::endl;
        return false;
    }

    try {
        json config;
        file >> config;

        if (!config.contains("plugins") || !config["plugins"].is_array()) {
            return true;  // 无插件配置，正常返回
        }

        for (const auto& pluginCfg : config["plugins"]) {
            if (!pluginCfg.contains("path")) continue;

            std::string pluginPath = pluginCfg["path"].get<std::string>();
            if (!loadPlugin(pluginPath)) {
                std::cerr << "[插件加载器] 加载插件失败: " << pluginPath << std::endl;
                // 继续加载其他插件
            }
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[插件加载器] 解析配置失败: " << e.what() << std::endl;
        return false;
    }
}
