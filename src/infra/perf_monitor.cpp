#include "src/infra/perf_monitor.h"
#include <sstream>
#include <iomanip>

PerformanceMonitor& PerformanceMonitor::get() {
    static PerformanceMonitor instance;
    return instance;
}

void PerformanceMonitor::recordOperation(const std::string& operation, int64_t elapsed_ms) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto& metric = _metrics[operation];
    metric.count++;
    metric.total_ms += elapsed_ms;
    metric.min_ms = elapsed_ms < metric.min_ms ? elapsed_ms : metric.min_ms;
    metric.max_ms = elapsed_ms > metric.max_ms ? elapsed_ms : metric.max_ms;
}

PerformanceMetric PerformanceMonitor::getMetrics(const std::string& operation) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _metrics.find(operation);
    return it != _metrics.end() ? it->second : PerformanceMetric{};
}

std::string PerformanceMonitor::getReport() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::ostringstream oss;
    oss << "[性能监控报告]\n";
    oss << std::fixed << std::setprecision(2);
    for (const auto& [op, metric] : _metrics) {
        oss << "  " << op << ": "
            << metric.count << " calls, "
            << "avg=" << metric.avg_ms() << "ms, "
            << "min=" << metric.min_ms << "ms, "
            << "max=" << metric.max_ms << "ms\n";
    }
    return oss.str();
}

void PerformanceMonitor::reset() {
    std::lock_guard<std::mutex> lock(_mutex);
    _metrics.clear();
}
