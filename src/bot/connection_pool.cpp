#include "src/bot/connection_pool.h"
#include <chrono>

ConnectionPool& ConnectionPool::get() {
    static ConnectionPool instance;
    return instance;
}

CURL* ConnectionPool::createConnection() {
    CURL* curl = curl_easy_init();
    if (curl) {
        // 设置默认选项
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
    }
    return curl;
}

void ConnectionPool::initialize(int poolSize) {
    std::lock_guard<std::mutex> lock(_mutex);
    _maxPoolSize = poolSize;
    _totalConnections = 0;

    for (int i = 0; i < poolSize; ++i) {
        CURL* curl = createConnection();
        if (curl) {
            _availableConnections.push({curl, false});
            _totalConnections++;
        }
    }

    std::cout << "[连接池] 初始化完成，池大小: " << _totalConnections << std::endl;
}

CURL* ConnectionPool::acquire(int timeoutMs) {
    std::unique_lock<std::mutex> lock(_mutex);

    // 等待可用连接
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (_availableConnections.empty() && _totalConnections < _maxPoolSize) {
        // 创建新连接
        CURL* curl = createConnection();
        if (curl) {
            _totalConnections++;
            _availableConnections.push({curl, false});
        } else {
            break;
        }
    }

    // 如果仍然没有可用连接，等待
    if (_availableConnections.empty()) {
        if (!_cv.wait_until(lock, deadline, [this]() { return !_availableConnections.empty(); })) {
            std::cerr << "[连接池] 获取连接超时" << std::endl;
            return nullptr;
        }
    }

    if (_availableConnections.empty()) {
        return nullptr;
    }

    auto conn = _availableConnections.front();
    _availableConnections.pop();
    conn.inUse = true;

    return conn.curl;
}

void ConnectionPool::release(CURL* curl) {
    if (!curl) return;

    std::lock_guard<std::mutex> lock(_mutex);
    _availableConnections.push({curl, false});
    _cv.notify_one();
}

void ConnectionPool::reset() {
    std::lock_guard<std::mutex> lock(_mutex);

    while (!_availableConnections.empty()) {
        auto conn = _availableConnections.front();
        _availableConnections.pop();
        if (conn.curl) {
            curl_easy_cleanup(conn.curl);
        }
    }

    _totalConnections = 0;
    std::cout << "[连接池] 已清空所有连接" << std::endl;
}

int ConnectionPool::getAvailableCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _availableConnections.size();
}

int ConnectionPool::getTotalCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _totalConnections;
}

ConnectionPool::~ConnectionPool() {
    reset();
}
