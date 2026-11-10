#pragma once

// 文件职责：全局日志器，保存最近的日志条目并可选输出到控制台。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：Logger 内部用互斥量保护条目，写日志可从任意线程调用；格式串须编译期合法。

#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace l3d {

// 日志级别，从最详细（Trace）到最严重（Error）；目前只用于标记，不做级别过滤。
enum class LogLevel : std::uint8_t { Trace, Info, Warning, Error };

// 一条日志记录：级别、格式化后的正文、本地时间的 "HH:MM:SS"。
struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string message;
    std::string timestamp;
};

// 全局单例日志器：保留最近 MaxEntries 条记录，供调试 UI 回看。
class Logger {
public:
    // 返回进程内唯一的实例（Meyers 单例，首次调用时构造，C++11 起初始化线程安全）。
    static Logger& instance();

    // 写入一条日志；会加锁，因此可从任意线程调用。
    void log(LogLevel level, std::string message);
    // 清空已保存的日志条目（不影响控制台开关）。
    void clear();
    // 是否同时打印到标准错误流。
    void setConsoleEnabled(bool enabled) noexcept { consoleEnabled_ = enabled; }

    // 返回日志副本：调用方可在锁外安全遍历，不会与后台写入冲突。
    [[nodiscard]] std::vector<LogEntry> snapshot() const;

    // 四个级别的便捷入口，用 std::format 语法格式化；格式串在编译期检查类型。
    // 例如 L3D_INFO("count = {}", n)。底层宏定义在文件末尾。
    template <typename... Args>
    void trace(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Trace, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void info(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warning(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void error(std::format_string<Args...> format, Args&&... args) {
        log(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
    }

private:
    // 内存中最多保留的条目数，超出后按块丢弃最旧的部分。
    static constexpr std::size_t MaxEntries = 2048;
    // mutable 让 const 的 snapshot() 也能加锁；所有对 entries_ 的访问都必须持锁。
    mutable std::mutex mutex_;
    std::vector<LogEntry> entries_;
    bool consoleEnabled_ = true;
};

[[nodiscard]] std::string_view toString(LogLevel level) noexcept;

} // namespace l3d

// 便捷宏：L3D_INFO("position = ({}, {})", x, y)。使用 :: 前缀避免与其他命名空间同名符号冲突。
// 参数按完美转发交给 std::format，每个实参只求值一次，可放心传临时对象。
#define L3D_TRACE(...) ::l3d::Logger::instance().trace(__VA_ARGS__)
#define L3D_INFO(...) ::l3d::Logger::instance().info(__VA_ARGS__)
#define L3D_WARN(...) ::l3d::Logger::instance().warning(__VA_ARGS__)
#define L3D_ERROR(...) ::l3d::Logger::instance().error(__VA_ARGS__)
