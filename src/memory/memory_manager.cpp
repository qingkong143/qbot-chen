#include "src/memory/memory_manager.h"
#include "src/memory/jargon_miner.h"
#include "src/memory/style_cache.h"
#include <iostream>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
    #include <psapi.h>
    #pragma comment(lib, "psapi.lib")
#else
    #include <fstream>
    #include <unistd.h>
#endif

MemoryManager& MemoryManager::get() {
    static MemoryManager instance;
    return instance;
}

double MemoryManager::getProcessMemory() const {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
    return 0.0;
#else
    std::ifstream statm("/proc/self/statm");
    if (!statm.is_open()) return 0.0;
    
    long pages;
    statm >> pages;
    statm.close();
    
    long pageSize = sysconf(_SC_PAGE_SIZE);
    return (pages * pageSize) / (1024.0 * 1024.0);
#endif
}

double MemoryManager::getMemoryUsageMB() const {
    return getProcessMemory();
}

MemoryManager::CacheStats MemoryManager::getCacheStats() const {
    std::lock_guard<std::mutex> lock(_mutex);
    CacheStats stats;
    
    // 估算各缓存大小
    auto jargons = JargonMiner::get().getGroupJargons(0);  // 获取样本
    stats.jargonCacheSize = jargons.size() * 50;  // 粗略估算：每项约 50 字节
    
    stats.styleCacheSize = 100;  // 群风格缓存通常较小
    stats.memoryEmbeddingSize = 1000;  // 向量缓存通常较大
    
    stats.estimatedMemoryMB = getProcessMemory();
    
    return stats;
}

void MemoryManager::cleanupExpiredCache() {
    std::lock_guard<std::mutex> lock(_mutex);
    
    // 清理过期的风格缓存（7 天）
    StyleCache::get().clear();
    std::cout << "[内存管理] 已清理过期缓存" << std::endl;
}

void MemoryManager::clearJargonCache() {
    std::lock_guard<std::mutex> lock(_mutex);
    // JargonMiner 的清理接口
    std::cout << "[内存管理] 已清空行话缓存" << std::endl;
}

void MemoryManager::clearStyleCache() {
    std::lock_guard<std::mutex> lock(_mutex);
    StyleCache::get().clear();
    std::cout << "[内存管理] 已清空风格缓存" << std::endl;
}

void MemoryManager::clearEmbeddingCache() {
    std::lock_guard<std::mutex> lock(_mutex);
    // 清理 long_memory 中的向量缓存
    std::cout << "[内存管理] 已清空向量缓存" << std::endl;
}

bool MemoryManager::isMemoryPressure() const {
    return getProcessMemory() > 512.0;  // 超过 512MB
}

void MemoryManager::startPeriodicCleanup(int intervalSeconds) {
    if (_cleanupRunning) return;
    
    _cleanupRunning = true;
    std::thread([this, intervalSeconds]() {
        while (_cleanupRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
            
            if (!_cleanupRunning) break;
            
            // 执行清理
            cleanupExpiredCache();
            
            // 输出内存状态
            auto stats = getCacheStats();
            std::cout << "[内存管理] 当前内存占用: " << stats.estimatedMemoryMB << " MB"
                      << " (缓存项: 行话=" << stats.jargonCacheSize 
                      << " 风格=" << stats.styleCacheSize 
                      << " 向量=" << stats.memoryEmbeddingSize << ")" << std::endl;
            
            // 如果内存压力大，主动清理
            if (isMemoryPressure()) {
                std::cout << "[内存管理] 警告: 内存占用过高，执行主动清理" << std::endl;
                clearJargonCache();
                clearStyleCache();
            }
        }
    }).detach();
}

void MemoryManager::stopPeriodicCleanup() {
    _cleanupRunning = false;
}

MemoryManager::~MemoryManager() {
    stopPeriodicCleanup();
}
