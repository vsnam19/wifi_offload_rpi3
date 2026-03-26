#pragma once

// logger.hpp — syslog wrapper using std::format (C++23)
// All log calls are async-signal-safe wrappers around syslog(3).

#include <format>
#include <syslog.h>

namespace logger {

namespace detail {

inline void log(int priority, const std::string& msg) {
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
    openlog(ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

inline void close() {
    closelog();
}

} // namespace logger
