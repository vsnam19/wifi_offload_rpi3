// path_state_fsm.cpp — Path State FSM implementation
//
//   P4-T1: FSM states and transitions
//   P4-T2: RSSI threshold checks (rssi.warn, rssi.drop read from config)

#include "fsm/path_state_fsm.hpp"
#include "common/logger.hpp"

#include <string_view>

namespace netservice {

// ── Constructor ───────────────────────────────────────────────────────────

PathStateFsm::PathStateFsm(const PathClassConfig& cfg, FsmStateCallback cb)
    : cfg_(cfg)
    , callback_(std::move(cb))
{}

// ── Public API ────────────────────────────────────────────────────────────

PathState PathStateFsm::currentState() const noexcept {
    return state_;
}

void PathStateFsm::onWpaEvent(const WpaEvent& ev) {
    switch (ev.type) {

        case WpaEventType::Connected:
            // wpa_supplicant confirmed association → PATH_UP.
            // Scanning/Connecting intermediate states are not driven in Phase 4
            // (no scan timer yet — that is Phase 3b / future).
            if (state_ != PathState::PathUp) {
                transitionTo(PathState::PathUp, ev.iface);
            }
            break;

        case WpaEventType::Disconnected:
        case WpaEventType::Terminating:
            if (state_ == PathState::PathUp || state_ == PathState::PathDegraded) {
                transitionTo(PathState::PathDown, ev.iface);
            }
            break;

        case WpaEventType::SignalChange:
            // RSSI changes only matter while the path is in service.
            if (state_ == PathState::PathUp || state_ == PathState::PathDegraded) {
                handleSignalChange(ev.rssi, ev.iface);
            }
            break;

        case WpaEventType::Other:
            break;
    }
}

// ── Private helpers ───────────────────────────────────────────────────────

void PathStateFsm::transitionTo(PathState newState, std::string_view iface) {
    logger::info("[FSM] {} → {}: class={} iface={}",
                 toString(state_), toString(newState), cfg_.id, iface);
    state_ = newState;
    if (callback_) {
        callback_(newState, iface);
    }
}

void PathStateFsm::handleSignalChange(int rssi, std::string_view iface) {
    if (state_ == PathState::PathDegraded && rssi < cfg_.rssi.drop) {
        // RSSI fell below drop threshold — path no longer usable.
        transitionTo(PathState::PathDown, iface);

    } else if (state_ == PathState::PathUp && rssi < cfg_.rssi.warn) {
        // RSSI fell below warn threshold — path degraded but still usable.
        transitionTo(PathState::PathDegraded, iface);

    } else if (state_ == PathState::PathDegraded && rssi >= cfg_.rssi.warn) {
        // RSSI recovered above warn threshold — path restored.
        transitionTo(PathState::PathUp, iface);
    }
    // else: no threshold crossed — no transition.
}

} // namespace netservice
