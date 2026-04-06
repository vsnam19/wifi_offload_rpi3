#pragma once

// path_state_fsm.hpp — Path State Finite State Machine
//
//   Phase 4 — P4-T1 / P4-T2
//
//   Tracks the connection state of one WiFi interface and drives routing /
//   MPTCP side-effects via an injectable StateCallback (for testability).
//
//   State transitions driven by WpaEvent (from WpaMonitor P3-T5 callback):
//     Connected   → PATH_UP    (from any non-up state)
//     Disconnected/Terminating → PATH_DOWN  (from PATH_UP or PATH_DEGRADED)
//     SignalChange(rssi < warn)  → PATH_DEGRADED  (from PATH_UP)
//     SignalChange(rssi >= warn) → PATH_UP         (from PATH_DEGRADED)
//     SignalChange(rssi < drop)  → PATH_DOWN       (from PATH_DEGRADED)
//
//   RSSI thresholds are read from PathClassConfig::rssi (set by ConfigLoader).
//
//   Scope boundary:
//     - Does NOT call RoutingPolicyManager or MptcpManager directly.
//       Callers inject side-effects via FsmStateCallback (main.cpp wires these
//       up to P4-T3/T4 route and MPTCP endpoint management).
//     - Does NOT notify consumers — that is Phase 5.
//     - Thread safety: NOT thread-safe; call from one thread (WpaMonitor thread).

#include "common/types.hpp"
#include "wpa/wpa_monitor.hpp"

#include <functional>
#include <string_view>

namespace netservice {

// Callback fired on every FSM state transition.
// newState: the state just entered.
// iface:    WiFi interface name from the triggering WpaEvent.
using FsmStateCallback =
    std::function<void(PathState newState, std::string_view iface)>;

// ── PathStateFsm ──────────────────────────────────────────────────────────
//
//   Usage:
//     PathStateFsm fsm{cfg,
//         [](PathState s, std::string_view iface) { /* side effects */ }};
//     fsm.onWpaEvent(wpaEvent);
//
class PathStateFsm {
public:
    // cfg: path class config — provides RSSI thresholds and class id for logs.
    //      Caller must ensure cfg outlives this FSM instance.
    // cb:  callback invoked on every state transition (may be nullptr).
    PathStateFsm(const PathClassConfig& cfg, FsmStateCallback cb);

    // Process one wpa_supplicant event; may trigger one or more state
    // transitions and fire the registered callback.
    void onWpaEvent(const WpaEvent& ev);

    // Query the current FSM state (used by tests and future Phase 5 API).
    [[nodiscard]] PathState currentState() const noexcept;

private:
    PathState            state_{PathState::Idle};
    const PathClassConfig& cfg_;   // non-owning — caller owns config lifetime
    FsmStateCallback     callback_;

    // Perform the transition, log it, and fire the callback.
    void transitionTo(PathState newState, std::string_view iface);

    // Handle RSSI-driven transitions (only valid in PATH_UP / PATH_DEGRADED).
    void handleSignalChange(int rssi, std::string_view iface);
};

} // namespace netservice
