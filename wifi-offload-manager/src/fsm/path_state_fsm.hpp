// path_state_fsm.hpp — P4-T1/P4-T2: Path State FSM for a single WiFi path class.
//
// Receives WpaEvent callbacks from WpaMonitor and transitions through
// PathState as described in docs/PROJECT_SUMMARY.md.
//
// Routing side-effects (addDefaultRoute / removeDefaultRoute) are injected
// as callbacks so the FSM has no direct dependency on RoutingPolicyManager —
// enabling clean unit testing without a kernel Netlink socket.

#pragma once

#include "common/types.hpp"
#include "wpa/wpa_monitor.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace netservice {

class PathStateFsm {
public:
    // ── Routing / MPTCP side-effects ────────────────────────────────────────
    // Callbacks triggered by state transitions.
    // iface  — the WiFi interface that changed (e.g., "eth0.100" in test)
    // table  — the routing table from PathClassConfig::routingTable
    struct Callbacks {
        // Called when FSM enters PathUp (or recovers PathDegraded → PathUp).
        // Implementation: addDefaultRoute + MPTCP endpoint (if mptcpEnabled).
        std::function<void(std::string_view iface, uint32_t table)> onPathUp;

        // Called when FSM enters PathDown (or PathDegraded → PathDown).
        // Implementation: removeDefaultRoute + MPTCP endpoint teardown.
        std::function<void(std::string_view iface, uint32_t table)> onPathDown;

        // Optional: observability hook — called after every state transition.
        // `iface` is the interface that drove the transition (may be empty for Terminating).
        // `rssi` is the RSSI from the triggering event (0 if not applicable).
        std::function<void(PathState from, PathState to,
                           std::string_view iface, int rssi)> onStateChanged;
    };

    // ── Construction ────────────────────────────────────────────────────────
    explicit PathStateFsm(PathClassConfig config, Callbacks callbacks) noexcept;

    // ── Event input ─────────────────────────────────────────────────────────
    // Feed a WpaEvent.  Computes the next state; if it differs from the
    // current state, fires the appropriate callback and records the transition.
    void onWpaEvent(const WpaEvent& ev);

    // ── Accessors ───────────────────────────────────────────────────────────
    [[nodiscard]] PathState currentState() const noexcept;
    [[nodiscard]] std::string_view activeIface() const noexcept;

    // ── Pure state-transition logic (P4-T5: exposed for unit tests) ─────────
    // Returns the next state given the current state, the event, and the
    // RSSI thresholds.  Has no side-effects.
    //
    // Transition table:
    //   Any state + Disconnected                  → PathDown
    //   Any state + Terminating                   → Idle
    //   Idle / PathDown + Connected, rssi ≥ warn  → PathUp
    //   Idle / PathDown + Connected, warn > rssi ≥ drop → PathDegraded
    //   Idle / PathDown + Connected, rssi < drop  → PathDown
    //   PathUp + SignalChange, rssi < warn         → PathDegraded
    //   PathDegraded + SignalChange, rssi ≥ warn   → PathUp
    //   PathDegraded + SignalChange, rssi < drop   → PathDown
    //   All other combinations                     → current (no change)
    [[nodiscard]] static PathState
    nextState(PathState current,
              const WpaEvent& ev,
              const RssiThresholds& thresholds) noexcept;

private:
    PathClassConfig config_;
    Callbacks       callbacks_;
    PathState       state_{PathState::Idle};
    std::string     activeIface_;   // interface that triggered last PathUp

    // Apply `newState`.  If it differs from `state_`, fires callbacks and
    // records the iface that was active at transition time.
    void transition(PathState newState, std::string_view iface, int rssi = 0);
};

} // namespace netservice
