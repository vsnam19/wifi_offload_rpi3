// test_path_state_fsm.cpp — Unit tests for PathStateFsm
// P4-T5: verify all FSM state transitions and RSSI threshold logic.

#include "fsm/path_state_fsm.hpp"
#include "common/types.hpp"
#include "wpa/wpa_monitor.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace netservice {
namespace {

// ── Helpers ───────────────────────────────────────────────────────

// Build a WpaEvent of the given type with optional iface / RSSI.
WpaEvent makeEvent(WpaEventType type,
                   std::string_view iface = "wlan0",
                   int rssi = 0)
{
    WpaEvent ev;
    ev.type   = type;
    ev.iface  = std::string{iface};
    ev.rssi   = rssi;
    ev.avgRssi = rssi;
    return ev;
}

// Default PathClassConfig for the multipath class (RSSI thresholds as spec).
PathClassConfig testConfig()
{
    PathClassConfig cfg;
    cfg.id              = "multipath";
    cfg.mptcpEnabled    = true;
    cfg.rssi.connectMin = -70;
    cfg.rssi.warn       = -75;
    cfg.rssi.drop       = -85;
    return cfg;
}

// Records every (state, iface) pair received by the FSM StateCallback.
struct TransitionLog {
    std::vector<PathState>   states;
    std::vector<std::string> ifaces;

    FsmStateCallback callback() {
        return [this](PathState s, std::string_view iface) {
            states.push_back(s);
            ifaces.push_back(std::string{iface});
        };
    }
};

// ── P4-T1: state transitions ──────────────────────────────────────

TEST(PathStateFsm, InitialStateIsIdle) {
    auto cfg = testConfig();
    PathStateFsm fsm{cfg, {}};
    EXPECT_EQ(fsm.currentState(), PathState::Idle);
}

TEST(PathStateFsm, ConnectedFromIdleGoesToPathUp) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));

    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    ASSERT_EQ(log.states.size(), 1u);
    EXPECT_EQ(log.states[0], PathState::PathUp);
    EXPECT_EQ(log.ifaces[0], "wlan0");
}

TEST(PathStateFsm, DisconnectedFromPathUpGoesToPathDown) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected));

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    ASSERT_EQ(log.states.size(), 2u);
    EXPECT_EQ(log.states[1], PathState::PathDown);
}

TEST(PathStateFsm, TerminatingFromPathUpGoesToPathDown) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Terminating));

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    ASSERT_EQ(log.states.size(), 2u);
    EXPECT_EQ(log.states[1], PathState::PathDown);
}

TEST(PathStateFsm, ReconnectFromPathDownGoesToPathUp) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));

    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    ASSERT_EQ(log.states.size(), 3u);
    EXPECT_EQ(log.states[2], PathState::PathUp);
}

TEST(PathStateFsm, DuplicateConnectedInPathUpIsIgnored) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Connected)); // duplicate

    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    EXPECT_EQ(log.states.size(), 1u); // only one transition fired
}

TEST(PathStateFsm, DisconnectedInIdleIsIgnored) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected));

    EXPECT_EQ(fsm.currentState(), PathState::Idle);
    EXPECT_TRUE(log.states.empty());
}

TEST(PathStateFsm, TerminatingInIdleIsIgnored) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Terminating));

    EXPECT_EQ(fsm.currentState(), PathState::Idle);
    EXPECT_TRUE(log.states.empty());
}

TEST(PathStateFsm, OtherEventIsIgnored) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Other));

    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    EXPECT_EQ(log.states.size(), 1u); // only the Connected transition
}

// ── P4-T2: RSSI threshold transitions ────────────────────────────

TEST(PathStateFsm, SignalBelowWarnGoesToPathDegraded) {
    auto cfg = testConfig(); // warn = -75
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -76)); // < -75

    EXPECT_EQ(fsm.currentState(), PathState::PathDegraded);
    ASSERT_EQ(log.states.size(), 2u);
    EXPECT_EQ(log.states[1], PathState::PathDegraded);
}

TEST(PathStateFsm, SignalAtExactWarnDoesNotDegrade) {
    auto cfg = testConfig(); // warn = -75
    PathStateFsm fsm{cfg, {}};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -75)); // == -75

    // rssi < warn requires strictly less-than; -75 is not < -75
    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
}

TEST(PathStateFsm, SignalRecoveryFromDegradedToPathUp) {
    auto cfg = testConfig(); // warn = -75
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -80)); // → Degraded
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -70)); // >= -75 → Up

    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    ASSERT_EQ(log.states.size(), 3u);
    EXPECT_EQ(log.states[1], PathState::PathDegraded);
    EXPECT_EQ(log.states[2], PathState::PathUp);
}

TEST(PathStateFsm, SignalAtExactWarnInDegradedRecovery) {
    auto cfg = testConfig(); // warn = -75
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -80)); // → Degraded
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -75)); // == warn → Up

    // rssi >= warn: -75 >= -75 is true → recovery
    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
}

TEST(PathStateFsm, SignalBelowDropFromDegradedGoesToPathDown) {
    auto cfg = testConfig(); // warn = -75, drop = -85
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -80)); // → Degraded
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -90)); // < -85 → Down

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    ASSERT_EQ(log.states.size(), 3u);
    EXPECT_EQ(log.states[2], PathState::PathDown);
}

TEST(PathStateFsm, SignalAtExactDropInDegradedDoesNotDrop) {
    auto cfg = testConfig(); // drop = -85
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -80)); // → Degraded
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -85)); // == drop

    // rssi < drop requires strictly less-than; -85 is not < -85
    // But -85 < warn (-75) is also false since we are in Degraded already.
    // Recovery check: -85 >= -75? No. So state stays Degraded.
    EXPECT_EQ(fsm.currentState(), PathState::PathDegraded);
}

TEST(PathStateFsm, SignalChangeBelowDropDirectlyFromPathUpGoesToDegraded) {
    // A very bad RSSI received while in PathUp triggers PathDegraded first,
    // not directly PathDown (one event → one threshold check).
    auto cfg = testConfig(); // warn = -75, drop = -85
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    // Single SignalChange event with rssi < drop while in PathUp:
    // handleSignalChange checks warn first → PathDegraded, then returns.
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -90));

    EXPECT_EQ(fsm.currentState(), PathState::PathDegraded);
    ASSERT_EQ(log.states.size(), 2u);
    EXPECT_EQ(log.states[1], PathState::PathDegraded);
}

TEST(PathStateFsm, SignalChangeInIdleIsIgnored) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -90));

    EXPECT_EQ(fsm.currentState(), PathState::Idle);
    EXPECT_TRUE(log.states.empty());
}

TEST(PathStateFsm, SignalChangeInPathDownIsIgnored) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected)); // → PathDown
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -90)); // ignored

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    EXPECT_EQ(log.states.size(), 2u);
}

TEST(PathStateFsm, DisconnectedFromPathDegradedGoesToPathDown) {
    auto cfg = testConfig();
    TransitionLog log;
    PathStateFsm fsm{cfg, log.callback()};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -80)); // → Degraded
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected));

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    ASSERT_EQ(log.states.size(), 3u);
    EXPECT_EQ(log.states[2], PathState::PathDown);
}

TEST(PathStateFsm, IfacePreservedThroughCallbackOnConnect) {
    auto cfg = testConfig();
    std::string capturedIface;
    PathStateFsm fsm{cfg, [&capturedIface](PathState, std::string_view iface) {
        capturedIface = std::string{iface};
    }};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected, "wlan0"));

    EXPECT_EQ(capturedIface, "wlan0");
}

TEST(PathStateFsm, IfacePreservedThroughCallbackOnDisconnect) {
    auto cfg = testConfig();
    std::string capturedIface;
    PathStateFsm fsm{cfg, [&capturedIface](PathState, std::string_view iface) {
        capturedIface = std::string{iface};
    }};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected, "wlan0"));
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected, "wlan0"));

    EXPECT_EQ(capturedIface, "wlan0");
}

TEST(PathStateFsm, NullCallbackDoesNotCrash) {
    auto cfg = testConfig();
    PathStateFsm fsm{cfg, {}}; // null callback

    // Should not crash even with no callback set
    fsm.onWpaEvent(makeEvent(WpaEventType::Connected));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "wlan0", -80));
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected));

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
}

} // namespace
} // namespace netservice

