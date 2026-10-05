#pragma once

// =====================================================================
// Zero-cost mode
// =====================================================================
//
// Define FILEXFERLIB_DISABLE_LOGGING to compile out all log statements.
// The FX_* macros expand to ((void)0) and the compiler removes them
// entirely. Code still compiles because the macros swallow everything.
//
// Define it in CMake with:
//     target_compile_definitions(filexfer PRIVATE FILEXFERLIB_DISABLE_LOGGING)
// Or in a specific TU before including this header:
//     #define FILEXFERLIB_DISABLE_LOGGING
//     #include <filexferlib/logger.hpp>

#ifdef FILEXFERLIB_DISABLE_LOGGING

    #define FX_TRACE(...) ((void)0)
    #define FX_DEBUG(...) ((void)0)
    #define FX_INFO(...)  ((void)0)
    #define FX_WARN(...)  ((void)0)
    #define FX_ERROR(...) ((void)0)

#else   // !FILEXFERLIB_DISABLE_LOGGING

#include <functional>
#include <string>
#include <sstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <atomic>
#include <cstdio>

namespace filexferlib {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

using LogSink = std::function<void(LogLevel, const std::string&)>;

class Logger {
public:
    static Logger& instance() {
        static Logger l;
        return l;
    }

    void setLevel(LogLevel lv) { level_ = lv; }
    LogLevel level() const { return level_; }

    void setSink(LogSink s) {
        std::lock_guard<std::mutex> lk(mu_);
        sink_ = std::move(s);
        hasSink_.store(static_cast<bool>(sink_), std::memory_order_relaxed);
    }

    // Fast, lock-free check. When false, log() calls are skipped and
    // the ostringstream is never constructed.
    bool hasSink() const {
        return hasSink_.load(std::memory_order_relaxed);
    }

    void log(LogLevel lv, const std::string& msg) {
        if (static_cast<int>(lv) < static_cast<int>(level_.load())) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (sink_) sink_(lv, msg);
    }

private:
    Logger() = default;

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<bool>     hasSink_{false};
    LogSink sink_;
    std::mutex mu_;
};

namespace detail {

inline const char* levelName(LogLevel lv) {
    switch (lv) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
    default: return "?";
    }
}

inline std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

template <typename... Args>
inline std::string fmt(Args&&... args) {
    std::ostringstream oss;
    (oss << ... << args);
    return oss.str();
}

} // namespace detail

} // namespace filexferlib

// Public macros.
//
// The hasSink() check comes FIRST so the ostringstream is never built
// when there is no sink. When no sink is registered and the level
// check passes, the whole expression is a single atomic load + branch.

#define FX_LOG(lv, ...) do {                                                   \
    auto& _fx_lg = ::filexferlib::Logger::instance();                             \
    if (_fx_lg.hasSink() &&                                                    \
        static_cast<int>(lv) >= static_cast<int>(_fx_lg.level()))              \
    {                                                                          \
        _fx_lg.log(                                                            \
            lv,                                                                \
            ::filexferlib::detail::fmt(::filexferlib::detail::timestamp(), " ",      \
                                    ::filexferlib::detail::levelName(lv), " ",    \
                                    __VA_ARGS__));                             \
    }                                                                          \
} while(0)

#define FX_TRACE(...) FX_LOG(::filexferlib::LogLevel::Trace, __VA_ARGS__)
#define FX_DEBUG(...) FX_LOG(::filexferlib::LogLevel::Debug, __VA_ARGS__)
#define FX_INFO(...)  FX_LOG(::filexferlib::LogLevel::Info,  __VA_ARGS__)
#define FX_WARN(...)  FX_LOG(::filexferlib::LogLevel::Warn,  __VA_ARGS__)
#define FX_ERROR(...) FX_LOG(::filexferlib::LogLevel::Error, __VA_ARGS__)

#endif  // FILEXFERLIB_DISABLE_LOGGING
