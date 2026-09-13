#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// 编译期日志等级控制：
// - 默认 LOG_ACTIVE_LEVEL = LOG_LEVEL_INFO，输出 INFO/WARN/ERROR。
// - 编译时可加 -DLOG_ACTIVE_LEVEL=LOG_LEVEL_WARN，只输出 WARN/ERROR。
// - 编译时可加 -DLOG_DISABLE，一键关闭所有 LOG_*。
//
// 等级数值越小越详细，越大越安静。
#define LOG_LEVEL_TRACE 0
#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARN 3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_OFF 5

#ifndef LOG_ACTIVE_LEVEL
#define LOG_ACTIVE_LEVEL LOG_LEVEL_INFO
#endif

#if defined(LOG_DISABLE)
#undef LOG_ACTIVE_LEVEL
#define LOG_ACTIVE_LEVEL LOG_LEVEL_OFF
#endif

namespace logx {

enum class Level {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

inline const char* levelName(Level level)
{
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

inline const char* shortFile(const char* path)
{
    if (path == nullptr)
        return "";

    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    return last;
}

inline std::string nowText()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto time = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm {};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
}

inline std::mutex& logMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline void write(Level level,
                  const char* module,
                  const char* file,
                  int line,
                  const char* function,
                  const std::string& message)
{
    (void)file;
    std::lock_guard<std::mutex> lock(logMutex());
    std::ostream& os = (level == Level::Error || level == Level::Warn) ? std::cerr : std::cout;
    os << nowText()
       << " [" << levelName(level) << "]"
       << " [" << (module ? module : "") << ":" << line << "]"
       << " " << (function ? function : "")
       << " => " << message << std::endl;
}

} // namespace logx

#define LOGX_WRITE(level, module, expr)                                               \
    do {                                                                              \
        std::ostringstream logx_oss;                                                  \
        logx_oss << expr;                                                             \
        ::logx::write(level, module, __FILE__, __LINE__, __func__, logx_oss.str());   \
    } while (false)

#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_TRACE
#define LOG_TRACE(module, expr) LOGX_WRITE(::logx::Level::Trace, module, expr)
#else
#define LOG_TRACE(module, expr) do { } while (false)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_DEBUG(module, expr) LOGX_WRITE(::logx::Level::Debug, module, expr)
#else
#define LOG_DEBUG(module, expr) do { } while (false)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_INFO
#define LOG_INFO(module, expr) LOGX_WRITE(::logx::Level::Info, module, expr)
#else
#define LOG_INFO(module, expr) do { } while (false)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_WARN
#define LOG_WARN(module, expr) LOGX_WRITE(::logx::Level::Warn, module, expr)
#else
#define LOG_WARN(module, expr) do { } while (false)
#endif

#if LOG_ACTIVE_LEVEL <= LOG_LEVEL_ERROR
#define LOG_ERROR(module, expr) LOGX_WRITE(::logx::Level::Error, module, expr)
#else
#define LOG_ERROR(module, expr) do { } while (false)
#endif
