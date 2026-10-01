#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace brazen {

// Central, thread-safe application log. Anything of note that happens in
// the app -- hardware detection, queued/started/finished runs, warnings,
// errors -- goes through here, and the UI's Log tab drains it every frame.
//
// It is deliberately header-only, has no dependency on ImGui, and can be
// written to from any thread (the benchmark worker thread, the GL/render
// thread, the UI thread, or before the UI exists at all -- entries simply
// queue up until the UI first drains them).
enum class LogLevel { Info, Warn, Error };

struct LogEntry {
    std::string timestamp; // "HH:MM:SS.mmm", local time
    LogLevel level = LogLevel::Info;
    std::string category;  // short subsystem tag, e.g. "Hardware", "Disk", "GPU"
    std::string message;
};

inline const char* ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

class Logger {
public:
    static Logger& Instance() {
        static Logger instance;
        return instance;
    }

    void Write(LogLevel level, const std::string& category, std::string message) {
        LogEntry entry;
        entry.timestamp = NowString();
        entry.level = level;
        entry.category = category;
        entry.message = std::move(message);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mirrorToStderr) {
            std::fprintf(stderr, "%s [%-5s] [%s] %s\n", entry.timestamp.c_str(), ToString(level),
                         entry.category.c_str(), entry.message.c_str());
        }
        m_pending.push_back(std::move(entry));
    }

    // Returns and clears every entry written since the last call.
    std::vector<LogEntry> Drain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<LogEntry> out;
        out.swap(m_pending);
        return out;
    }

    // Also echo entries to stderr (handy when launched from a terminal
    // during development). On by default; harmless when there is no
    // console attached, such as a double-clicked Windows GUI build.
    void SetMirrorToStderr(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mirrorToStderr = enabled;
    }

private:
    Logger() = default;

    static std::string NowString() {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t t = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tmBuf.tm_hour, tmBuf.tm_min,
                      tmBuf.tm_sec, static_cast<int>(ms));
        return buf;
    }

    std::mutex m_mutex;
    std::vector<LogEntry> m_pending;
    bool m_mirrorToStderr = true;
};

#if defined(__MINGW32__)
    // MinGW's g++ uses the C99-conforming printf family (so %zu/%llu work),
    // which the compiler's format checker only knows as "gnu_printf".
    #define BRAZEN_PRINTF_LIKE(fmtIdx, argIdx) __attribute__((format(gnu_printf, fmtIdx, argIdx)))
#elif defined(__GNUC__) || defined(__clang__)
    #define BRAZEN_PRINTF_LIKE(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
    #define BRAZEN_PRINTF_LIKE(fmtIdx, argIdx)
#endif

namespace detail {
inline void LogV(LogLevel level, const char* category, const char* fmt, va_list args) {
    char stackBuf[512];
    va_list copy;
    va_copy(copy, args);
    int needed = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
    std::string message;
    if (needed < 0) {
        message = fmt; // formatting failed; better to log the raw format than nothing
    } else if (static_cast<size_t>(needed) < sizeof(stackBuf)) {
        message.assign(stackBuf, static_cast<size_t>(needed));
    } else {
        message.resize(static_cast<size_t>(needed) + 1);
        std::vsnprintf(&message[0], message.size(), fmt, copy);
        message.resize(static_cast<size_t>(needed));
    }
    va_end(copy);
    Logger::Instance().Write(level, category, std::move(message));
}
} // namespace detail

// printf-style convenience wrappers: LogInfo("Disk", "Wrote %d MB", mb);
inline void LogInfo(const char* category, const char* fmt, ...) BRAZEN_PRINTF_LIKE(2, 3);
inline void LogInfo(const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::LogV(LogLevel::Info, category, fmt, args);
    va_end(args);
}

inline void LogWarn(const char* category, const char* fmt, ...) BRAZEN_PRINTF_LIKE(2, 3);
inline void LogWarn(const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::LogV(LogLevel::Warn, category, fmt, args);
    va_end(args);
}

inline void LogError(const char* category, const char* fmt, ...) BRAZEN_PRINTF_LIKE(2, 3);
inline void LogError(const char* category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::LogV(LogLevel::Error, category, fmt, args);
    va_end(args);
}

} // namespace brazen
