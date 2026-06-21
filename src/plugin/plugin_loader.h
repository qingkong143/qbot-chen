#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include "src/infra/event_bus.h"

// 插件接口
class IPlugin {
public:
    virtual ~IPlugin() = default;

    // 插件初始化
    virtual bool init() = 0;

    // 插件销毁
    virtual void destroy() = 0;

    // 获取插件名称
    virtual std::string getName() const = 0;

    // 获取插件版本
    virtual std::string getVersion() const = 0;

    // 注册事件处理器
    virtual void registerHandlers(EventBus& bus) = 0;
};

// 插件信息
struct PluginInfo {
    std::string name;
    std::string path;
    std::string version;
    bool enabled = false;
    void* handle = nullptr;  // 动态库句柄
    IPlugin* instance = nullptr;
};

// 插件加载器
class PluginLoader {
public:
    static PluginLoader& get();

    // 从配置加载所有插件
    bool loadPluginsFromConfig(const std::string& configPath);

    // 加载单个插件
    bool loadPlugin(const std::string& pluginPath);

    // 卸载单个插件
    bool unloadPlugin(const std::string& pluginName);

    // 卸载所有插件
    void unloadAll();

    // 获取已加载的插件列表
    std::vector<PluginInfo> getLoadedPlugins() const;

    // 获取插件实例
    IPlugin* getPlugin(const std::string& pluginName) const;

private:
    PluginLoader() = default;

    std::map<std::string, PluginInfo> _plugins;
    mutable std::mutex _mutex;

    // 平台相关的加载函数
    void* loadLibrary(const std::string& path);
    void unloadLibrary(void* handle);
    void* getSymbol(void* handle, const std::string& symbol);
};
