// 文件职责：Logger 的实现，负责加时间戳、限制条目数量并输出到控制台。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：entries_ 必须在持锁状态下访问；控制台输出位于锁外，避免慢 IO 阻塞其他线程。

#include "l3d/core/logging.h"

#include <chrono>
#include <ctime>
#include <iostream>

namespace l3d {

namespace {
// 取本地时间并格式化为 "HH:MM:SS"。
// 注意不能用 localtime()：它返回共享的静态缓冲，多线程调用会互相覆盖；
// 下面两个平台专用版本才是线程安全的。
std::string currentTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}
} // namespace

Logger& Logger::instance() {
    // 函数内静态对象：首次调用时构造，C++11 保证这个初始化是线程安全的。
    static Logger logger;
    return logger;
}

void Logger::log(LogLevel level, std::string message) {
    // 时间戳在锁外生成，缩短临界区；entry 只在当前线程使用。
    LogEntry entry{level, std::move(message), currentTimestamp()};
    {
        std::lock_guard lock(mutex_);
        // 满了就一次丢掉最旧的 1/4，而不是每来一条都 erase 一个：
        // 批量删除把均摊开销降到 O(1)，否则每帧大量日志会变成 O(n) 搬运。
        if (entries_.size() >= MaxEntries) {
            entries_.erase(entries_.begin(), entries_.begin() + MaxEntries / 4);
        }
        entries_.push_back(entry);
    }
    // 控制台输出放在锁外：IO 较慢，不该让其他线程排队等它；
    // 代价是两个线程的日志行可能交错，但不会破坏数据。
    if (consoleEnabled_) {
        std::clog << '[' << entry.timestamp << "] [" << toString(entry.level) << "] "
                  << entry.message << '\n';
    }
}

void Logger::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

std::vector<LogEntry> Logger::snapshot() const {
    // 返回拷贝：调用方拿到后可以慢慢遍历，期间其他线程写日志也不会与之冲突。
    std::lock_guard lock(mutex_);
    return entries_;
}

std::string_view toString(LogLevel level) noexcept {
    // 返回的是静态字符串字面量，生命周期贯穿整个程序，无需拷贝。
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

} // namespace l3d
