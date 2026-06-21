#include "src/infra/event_bus.h"
#include <iostream>
#include <thread>
#include <chrono>

EventBus& EventBus::get() {
    static EventBus instance;
    return instance;
}

void EventBus::subscribeIntercept(EventType type, const std::string& name, EventHandler handler) {
    std::lock_guard<std::mutex> lock(_mutex);
    _interceptHandlers[type].push_back({name, handler});
}

void EventBus::subscribeAsync(EventType type, const std::string& name, EventHandler handler) {
    std::lock_guard<std::mutex> lock(_mutex);
    _asyncHandlers[type].push_back({name, handler});
}

bool EventBus::fireIntercept(const Event& event) {
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_interceptHandlers.count(event.type)) {
            handlers = _interceptHandlers[event.type];
        }
    }

    for (const auto& handler : handlers) {
        if (!handler.func(event)) {
            return false;  // 中断传播
        }
    }
    return true;  // 完成传播
}

void EventBus::fireAsync(const Event& event) {
    std::vector<Handler> handlers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_asyncHandlers.count(event.type)) {
            handlers = _asyncHandlers[event.type];
        }
    }

    // 在后台线程执行（fire-and-forget）
    std::thread([handlers, event]() {
        for (const auto& handler : handlers) {
            try {
                handler.func(event);
            } catch (...) {
                std::cerr << "[事件总线] 异步处理器异常: " << handler.name << std::endl;
            }
        }
    }).detach();
}

void EventBus::unsubscribe(EventType type, const std::string& name) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_interceptHandlers.count(type)) {
        auto& handlers = _interceptHandlers[type];
        handlers.erase(
            std::remove_if(handlers.begin(), handlers.end(),
                          [&name](const Handler& h) { return h.name == name; }),
            handlers.end());
    }

    if (_asyncHandlers.count(type)) {
        auto& handlers = _asyncHandlers[type];
        handlers.erase(
            std::remove_if(handlers.begin(), handlers.end(),
                          [&name](const Handler& h) { return h.name == name; }),
            handlers.end());
    }
}

void EventBus::clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _interceptHandlers.clear();
    _asyncHandlers.clear();
}

int EventBus::getHandlerCount(EventType type) const {
    std::lock_guard<std::mutex> lock(_mutex);
    int count = 0;
    if (_interceptHandlers.count(type)) count += _interceptHandlers.at(type).size();
    if (_asyncHandlers.count(type)) count += _asyncHandlers.at(type).size();
    return count;
}
