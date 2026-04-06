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
    // ctrl_ is always cleaned up inside start()'s reconnect loop.
    // This guard handles the edge case where stop() is called before start().
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
    stopRequested_.store(false, std::memory_order_release);
    auto backoff = kInitialBackoff;

    // P6-T2: reconnect loop — retries indefinitely until stop() is called.
    while (!stopRequested_.load(std::memory_order_acquire)) {
        ctrl_ = wpa_ctrl_open(ctrlPath_.c_str());
        if (!ctrl_) {
            logger::warn("[WPA] ctrl_open failed: iface={} path={} — retry in {}s",
                         iface_, ctrlPath_, backoff.count());
        } else if (wpa_ctrl_attach(ctrl_) != 0) {
            logger::warn("[WPA] attach failed: iface={} — retry in {}s",
                         iface_, backoff.count());
            wpa_ctrl_close(ctrl_);
            ctrl_ = nullptr;
        } else {
            logger::info("[WPA] attached: iface={} path={}", iface_, ctrlPath_);
            backoff = kInitialBackoff;  // reset on successful connection

            runEventLoop();

            wpa_ctrl_detach(ctrl_);
            wpa_ctrl_close(ctrl_);
            ctrl_ = nullptr;

            if (stopRequested_.load(std::memory_order_acquire)) break;
            logger::warn("[WPA] connection lost: iface={} — retry in {}s",
                         iface_, backoff.count());
        }

        if (stopRequested_.load(std::memory_order_acquire)) break;

        // Interruptible backoff sleep: stop() calls cv_.notify_all()
        {
            std::unique_lock lock{cvMutex_};
            cv_.wait_for(lock, backoff,
                         [this]{ return stopRequested_.load(std::memory_order_acquire); });
        }
        backoff = std::min(backoff * 2, kMaxBackoff);
    }

    logger::info("[WPA] stopped: iface={}", iface_);
    return {};
}

void WpaMonitor::stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    cv_.notify_all();  // interrupt any backoff sleep immediately
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

    while (!stopRequested_.load(std::memory_order_acquire)) {
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
            const bool done = handleEvent(std::string_view{buf, len});
            if (done) break;  // Terminating or unrecoverable event
        } else {
            logger::error("[WPA] wpa_ctrl_recv error: iface={}", iface_);
            break;
        }
    }

    logger::info("[WPA] event loop exited: iface={}", iface_);
}

bool WpaMonitor::handleEvent(std::string_view raw) {
    const WpaEvent ev = parseEvent(raw, iface_);
    bool exitLoop = false;

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
            exitLoop = true;  // exit runEventLoop; reconnect loop will retry
            break;
        case WpaEventType::Other:
            // Intentionally not logged at info level — too noisy
            logger::debug("[WPA] event ignored: {}", ev.raw);
            break;
    }

    if (callback_) {
        callback_(ev);
    }
    return exitLoop;
}

} // namespace netservice
