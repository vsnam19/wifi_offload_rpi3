#pragma once

// wpa_monitor.hpp — wpa_supplicant event monitor
//
//   Connects to wpa_supplicant's control socket, attaches as event monitor,
//   and dispatches parsed WiFi events to a registered callback.
//
//   Implemented tasks:
//     P3-T2 — wpa_ctrl connect / attach / event loop
//     P3-T3 — Parse CONNECTED, DISCONNECTED, SIGNAL-CHANGE events
//     P3-T4 — Extract RSSI from CTRL-EVENT-SIGNAL-CHANGE
//     P3-T5 — Callback interface → feeds events into FSM (Phase 4)
//
//   Scope boundary:
//     - Monitor events only — does NOT manage wpa_supplicant lifecycle
//     - Does NOT implement scan/connect (Phase 3b, future)
//     - wpa_ctrl.h is NOT modified

#include "common/error.hpp"
#include "common/types.hpp"

extern "C" {
#include "wpa/wpa_ctrl.h"
}

#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace netservice {

// ── Parsed wpa event types ────────────────────────────────────────

enum class WpaEventType {
    Connected,         // CTRL-EVENT-CONNECTED
    Disconnected,      // CTRL-EVENT-DISCONNECTED
    SignalChange,      // CTRL-EVENT-SIGNAL-CHANGE
    Terminating,       // CTRL-EVENT-TERMINATING (wpa_supplicant shutting down)
    Other,             // all other events — ignored
};

struct WpaEvent {
    WpaEventType type{WpaEventType::Other};
    std::string  raw;           // full event string (for debug)
    std::string  iface;         // interface name (set by WpaMonitor)

    // Populated only for SignalChange events
    int rssi{0};                // current RSSI (dBm)
    int avgRssi{0};             // average RSSI (dBm)
};

// Callback type: called on every parsed event (P3-T5 interface to FSM)
using WpaEventCallback = std::function<void(const WpaEvent&)>;

// ── WpaMonitor ────────────────────────────────────────────────────
//
//   Usage:
//     WpaMonitor mon("/var/run/wpa_supplicant/wlan0", "wlan0");
//     mon.setEventCallback([](const WpaEvent& e){ ... });
//     mon.start();     // blocks until stop() is called or wpa_supplicant dies
//     mon.stop();      // thread-safe: sets stop flag, event loop exits
//
class WpaMonitor {
public:
    // ctrl_path: path to wpa_supplicant control socket dir
    //   e.g. "/var/run/wpa_supplicant/wlan0"
    // iface: interface name for logging and WpaEvent::iface field
    WpaMonitor(std::string ctrlPath, std::string iface);
    ~WpaMonitor();

    // Non-copyable, non-movable (holds raw C pointer to wpa_ctrl)
    WpaMonitor(const WpaMonitor&)            = delete;
    WpaMonitor& operator=(const WpaMonitor&) = delete;
    WpaMonitor(WpaMonitor&&)                 = delete;
    WpaMonitor& operator=(WpaMonitor&&)      = delete;

    // Register callback invoked on every parsed event.
    // Must be called before start().
    void setEventCallback(WpaEventCallback cb);

    // Connect, attach, and run the event loop.
    // Blocks the calling thread until stop() is called or wpa_supplicant
    // terminates (CTRL-EVENT-TERMINATING received).
    // Returns WpaError::ConnectFailed / AttachFailed on setup failure.
    [[nodiscard]] std::expected<void, WpaError> start();

    // Signal the event loop to exit. Thread-safe.
    void stop() noexcept;

    // Parse a raw wpa_supplicant event string → WpaEvent
    // Exposed as static for unit testing.
    [[nodiscard]] static WpaEvent parseEvent(std::string_view raw, std::string_view iface);

private:
    std::string       ctrlPath_;
    std::string       iface_;
    WpaEventCallback  callback_;
    struct wpa_ctrl*  ctrl_{nullptr};
    volatile bool     stopRequested_{false};

    static constexpr std::size_t kRecvBufSize{4096};
    static constexpr int         kPollTimeoutMs{500};

    void runEventLoop();
    void handleEvent(std::string_view raw);
};

} // namespace netservice
