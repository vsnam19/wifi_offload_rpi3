// wpa_monitor.cpp — wpa_supplicant event monitor implementation
//
//   P3-T2: wpa_ctrl connect / attach / event loop
//   P3-T3: Parse CONNECTED, DISCONNECTED, SIGNAL-CHANGE events
//   P3-T4: Extract RSSI integer from CTRL-EVENT-SIGNAL-CHANGE rssi=X avg_rssi=Y
//   P3-T5: Callback interface → feeds events into FSM (Phase 4)

#include "wpa/wpa_monitor.hpp"
#include "common/logger.hpp"

extern "C" {
#include "wpa/wpa_ctrl.h"
}

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <poll.h>
#include <string>
#include <string_view>

namespace netservice {

// ── Constructor / Destructor ──────────────────────────────────────

WpaMonitor::WpaMonitor(std::string ctrlPath, std::string iface)
    : ctrlPath_(std::move(ctrlPath))
    , iface_(std::move(iface))
{}

WpaMonitor::~WpaMonitor() {
    stop();
    if (ctrl_) {
        wpa_ctrl_detach(ctrl_);
        wpa_ctrl_close(ctrl_);
        ctrl_ = nullptr;
    }
}

// ── Public API ────────────────────────────────────────────────────

void WpaMonitor::setEventCallback(WpaEventCallback cb) {
    callback_ = std::move(cb);
}

std::expected<void, WpaError> WpaMonitor::start() {
    ctrl_ = wpa_ctrl_open(ctrlPath_.c_str());
    if (!ctrl_) {
        logger::error("[WPA] failed to open ctrl socket: path={} errno={} ({})",
                      ctrlPath_, errno, strerror(errno));
        return std::unexpected(WpaError::ConnectFailed);
    }

    if (wpa_ctrl_attach(ctrl_) != 0) {
        logger::error("[WPA] wpa_ctrl_attach failed: iface={}", iface_);
        wpa_ctrl_close(ctrl_);
        ctrl_ = nullptr;
        return std::unexpected(WpaError::AttachFailed);
    }

    logger::info("[WPA] attached to wpa_supplicant: iface={} path={}", iface_, ctrlPath_);

    stopRequested_ = false;
    runEventLoop();
    return {};
}

void WpaMonitor::stop() noexcept {
    stopRequested_ = true;
}

// ── Event parsing (static — unit-testable) ────────────────────────

// Strip the priority prefix from wpa_supplicant messages.
// wpa events arrive prefixed with "<N>" e.g. "<3>CTRL-EVENT-CONNECTED ..."
static std::string_view stripPriority(std::string_view raw) noexcept {
    if (!raw.empty() && raw.front() == '<') {
        const auto close = raw.find('>');
        if (close != std::string_view::npos) {
            return raw.substr(close + 1);
        }
    }
    return raw;
}

// Parse "rssi=X avg_rssi=Y" fields.
// Returns 0 if the key is not found.
static int parseIntField(std::string_view msg, std::string_view key) noexcept {
    const auto pos = msg.find(key);
    if (pos == std::string_view::npos) return 0;

    const auto start = pos + key.size();
    if (start >= msg.size()) return 0;

    // Value extends until the next space or end-of-string
    const auto end = msg.find(' ', start);
    const std::string_view valStr = (end == std::string_view::npos)
        ? msg.substr(start)
        : msg.substr(start, end - start);

    int value{0};
    std::from_chars(valStr.data(), valStr.data() + valStr.size(), value);
    return value;
}

/*static*/ WpaEvent WpaMonitor::parseEvent(std::string_view raw, std::string_view iface) {
    WpaEvent ev;
    ev.raw   = std::string{raw};
    ev.iface = std::string{iface};

    const std::string_view msg = stripPriority(raw);

    if (msg.starts_with(WPA_EVENT_CONNECTED)) {
        ev.type = WpaEventType::Connected;

    } else if (msg.starts_with(WPA_EVENT_DISCONNECTED)) {
        ev.type = WpaEventType::Disconnected;

    } else if (msg.starts_with(WPA_EVENT_SIGNAL_CHANGE)) {
        ev.type   = WpaEventType::SignalChange;
        ev.rssi    = parseIntField(msg, "rssi=");
        ev.avgRssi = parseIntField(msg, "avg_rssi=");

    } else if (msg.starts_with(WPA_EVENT_TERMINATING)) {
        ev.type = WpaEventType::Terminating;

    } else {
        ev.type = WpaEventType::Other;
    }

    return ev;
}

// ── Internal event loop ───────────────────────────────────────────

void WpaMonitor::runEventLoop() {
    const int fd = wpa_ctrl_get_fd(ctrl_);

    struct pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLIN;

    char buf[kRecvBufSize];

    while (!stopRequested_) {
        const int ret = poll(&pfd, 1, kPollTimeoutMs);

        if (ret < 0) {
            if (errno == EINTR) continue;
            logger::error("[WPA] poll error: iface={} errno={} ({})",
                          iface_, errno, strerror(errno));
            break;
        }

        if (ret == 0) continue; // timeout — check stopRequested_

        if (!(pfd.revents & POLLIN)) continue;

        size_t len = sizeof(buf) - 1;
        if (wpa_ctrl_recv(ctrl_, buf, &len) == 0) {
            buf[len] = '\0';
            handleEvent(std::string_view{buf, len});
        } else {
            logger::error("[WPA] wpa_ctrl_recv error: iface={}", iface_);
            break;
        }
    }

    logger::info("[WPA] event loop exited: iface={}", iface_);
}

void WpaMonitor::handleEvent(std::string_view raw) {
    const WpaEvent ev = parseEvent(raw, iface_);

    switch (ev.type) {
        case WpaEventType::Connected:
            logger::info("[WPA] CONNECTED: iface={} raw={}", iface_, ev.raw);
            break;
        case WpaEventType::Disconnected:
            logger::info("[WPA] DISCONNECTED: iface={}", iface_);
            break;
        case WpaEventType::SignalChange:
            logger::debug("[WPA] SIGNAL-CHANGE: iface={} rssi={} avg_rssi={}",
                          iface_, ev.rssi, ev.avgRssi);
            break;
        case WpaEventType::Terminating:
            logger::warn("[WPA] TERMINATING: wpa_supplicant exiting on iface={}", iface_);
            stopRequested_ = true;
            break;
        case WpaEventType::Other:
            // Intentionally not logged at info level — too noisy
            logger::debug("[WPA] event ignored: {}", ev.raw);
            break;
    }

    if (callback_) {
        callback_(ev);
    }
}

} // namespace netservice
