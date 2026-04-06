// path_state_fsm.cpp — P4-T1 / P4-T2: Path State FSM for a single WiFi path class.

#include "fsm/path_state_fsm.hpp"
#include "common/logger.hpp"

#include <format>
#include <string>

namespace netservice {

namespace {

constexpr std::string_view stateToStr(PathState s) noexcept {
    switch (s) {
        case PathState::Idle:         return "Idle";
        case PathState::Scanning:     return "Scanning";
        case PathState::Connecting:   return "Connecting";
        case PathState::PathUp:       return "PathUp";
        case PathState::PathDegraded: return "PathDegraded";
        case PathState::PathDown:     return "PathDown";
    }
    return "Unknown";
}

// Returns true if the state is one where we do NOT yet have a live route
// installed (i.e., the path was not "up" in any form).
constexpr bool isDownState(PathState s) noexcept {
    return s == PathState::Idle || s == PathState::PathDown ||
           s == PathState::Scanning || s == PathState::Connecting;
}

} // anonymous namespace

// ── Construction ─────────────────────────────────────────────────────────────
PathStateFsm::PathStateFsm(PathClassConfig config, Callbacks callbacks) noexcept
    : config_{std::move(config)}, callbacks_{std::move(callbacks)}
{}

// ── Accessors ─────────────────────────────────────────────────────────────────
PathState PathStateFsm::currentState() const noexcept { return state_; }

std::string_view PathStateFsm::activeIface() const noexcept { return activeIface_; }

// ── Pure state machine (P4-T1 + P4-T2) ───────────────────────────────────────
PathState PathStateFsm::nextState(PathState current,
                                  const WpaEvent& ev,
                                  const RssiThresholds& thresholds) noexcept
{
    switch (ev.type) {
        case WpaEventType::Terminating:
            return PathState::Idle;

        case WpaEventType::Disconnected:
            return PathState::PathDown;

        case WpaEventType::Connected:
            // Only lift from a "down" state.
            if (isDownState(current)) {
                if (ev.rssi >= thresholds.warn) {
                    return PathState::PathUp;
                }
                if (ev.rssi >= thresholds.drop) {
                    return PathState::PathDegraded;
                }
                return PathState::PathDown;
            }
            return current;

        case WpaEventType::SignalChange:
            if (current == PathState::PathUp) {
                if (ev.rssi < thresholds.warn) {
                    return PathState::PathDegraded;
                }
                return PathState::PathUp;  // no change
            }
            if (current == PathState::PathDegraded) {
                if (ev.rssi >= thresholds.warn) {
                    return PathState::PathUp;
                }
                if (ev.rssi < thresholds.drop) {
                    return PathState::PathDown;
                }
                return PathState::PathDegraded;  // still degraded
            }
            return current;

        case WpaEventType::Other:
        default:
            return current;
    }
}

// ── transition ────────────────────────────────────────────────────────────────
void PathStateFsm::transition(PathState newState, std::string_view iface) {
    if (newState == state_) return;

    const PathState prev = state_;
    logger::info("[FSM] {} state: {} → {} (iface={})",
                 config_.id, stateToStr(prev), stateToStr(newState), iface);

    state_ = newState;

    // ── side-effects ─────────────────────────────────────────────────────
    // PathUp entry: install default route (and MPTCP endpoints via caller).
    if (newState == PathState::PathUp) {
        activeIface_ = std::string{iface};
        if (callbacks_.onPathUp) {
            callbacks_.onPathUp(activeIface_, config_.routingTable);
        }
    }
    // PathDown entry (from a "live" state): tear down default route.
    else if (newState == PathState::PathDown &&
             (prev == PathState::PathUp || prev == PathState::PathDegraded)) {
        if (callbacks_.onPathDown && !activeIface_.empty()) {
            callbacks_.onPathDown(activeIface_, config_.routingTable);
        }
        activeIface_.clear();
    }
    // PathDegraded entry from PathUp: route stays; no route callback.
    // PathDegraded → PathDown: covered by the PathDown branch above.

    if (callbacks_.onStateChanged) {
        callbacks_.onStateChanged(prev, newState);
    }
}

// ── onWpaEvent ────────────────────────────────────────────────────────────────
void PathStateFsm::onWpaEvent(const WpaEvent& ev) {
    const PathState next = nextState(state_, ev, config_.rssi);
    transition(next, ev.iface);
}

} // namespace netservice
