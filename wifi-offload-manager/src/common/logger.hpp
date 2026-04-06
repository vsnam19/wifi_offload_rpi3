#pragma once

// logger.hpp — dual-sink logger: syslog(3) + stderr (C++23)
//
// Writing to stderr is the primary journal path when running under systemd:
//   StandardOutput=journal / StandardError=journal in the .service file
//   routes stderr directly into journald without going through the syslog
//   socket.  This avoids conflicts with busybox-syslog on Yocto images.
// syslog() is kept as a secondary sink so the same binary also works outside
// of systemd (e.g. run manually in a shell).

#include <cstdio>
#include <format>
#include <syslog.h>

namespace logger {

namespace detail {

// Map syslog priority → short human-readable prefix for stderr output.
constexpr const char* priorityTag(int priority) noexcept {
    switch (priority) {
        case LOG_DEBUG:   return "DBG";
        case LOG_INFO:    return "INF";
        case LOG_WARNING: return "WRN";
        case LOG_ERR:     return "ERR";
        default:          return "   ";
    }
}

inline void log(int priority, const std::string& msg) {
    // stderr → journald (via StandardError=journal in the service unit)
    std::fprintf(stderr, "[%s] %s\n", priorityTag(priority), msg.c_str());
    // syslog → fallback for non-systemd environments
    syslog(priority, "%s", msg.c_str());
}

} // namespace detail

template<typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    detail::log(LOG_DEBUG, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    detail::log(LOG_INFO, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    detail::log(LOG_WARNING, std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    detail::log(LOG_ERR, std::format(fmt, std::forward<Args>(args)...));
}

inline void open(const char* ident) {
    // Line-buffer stderr so each log line flushes immediately to journald.
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

inline void close() {
    closelog();
}

} // namespace logger
