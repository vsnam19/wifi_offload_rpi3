// test_path_state_fsm.cpp — P4-T5: unit tests for PathStateFsm

#include "fsm/path_state_fsm.hpp"
#include <gtest/gtest.h>

using namespace netservice;

// ── Helper builders ──────────────────────────────────────────────────────────

static PathClassConfig makeConfig(bool mptcpEnabled = false,
                                  uint32_t routingTable = 100) {
    PathClassConfig cfg;
    cfg.id            = "test_class";
    cfg.classid       = 0x00010001u;
    cfg.cgroupPath    = "/sys/fs/cgroup/net_cls/test_class";
    cfg.interfaces    = {"eth0.100"};
    cfg.mptcpEnabled  = mptcpEnabled;
    cfg.routingTable  = routingTable;
    cfg.mark          = 0x10u;
    cfg.strictIsolation = false;
    // Default thresholds: connectMin=-70, warn=-75, drop=-85
    return cfg;
}

static WpaEvent makeEvent(WpaEventType type,
                          std::string_view iface = "eth0.100",
                          int rssi = 0) {
    WpaEvent ev;
    ev.type    = type;
    ev.iface   = std::string{iface};
    ev.rssi    = rssi;
    ev.avgRssi = rssi;
    return ev;
}

// ── PathStateFsm::nextState — pure state machine tests ───────────────────────

TEST(PathStateFsmNextState, TerminatingFromAnyState_GivesIdle) {
    const RssiThresholds t;
    const WpaEvent ev = makeEvent(WpaEventType::Terminating);

    EXPECT_EQ(PathState::Idle,
              PathStateFsm::nextState(PathState::PathUp, ev, t));
    EXPECT_EQ(PathState::Idle,
              PathStateFsm::nextState(PathState::PathDegraded, ev, t));
    EXPECT_EQ(PathState::Idle,
              PathStateFsm::nextState(PathState::PathDown, ev, t));
    EXPECT_EQ(PathState::Idle,
              PathStateFsm::nextState(PathState::Idle, ev, t));
}

TEST(PathStateFsmNextState, DisconnectedFromAnyState_GivesPathDown) {
    const RssiThresholds t;
    const WpaEvent ev = makeEvent(WpaEventType::Disconnected);

    EXPECT_EQ(PathState::PathDown,
              PathStateFsm::nextState(PathState::PathUp, ev, t));
    EXPECT_EQ(PathState::PathDown,
              PathStateFsm::nextState(PathState::PathDegraded, ev, t));
    EXPECT_EQ(PathState::PathDown,
              PathStateFsm::nextState(PathState::Idle, ev, t));
}

TEST(PathStateFsmNextState, ConnectedGoodRssi_GivesPathUp) {
    const RssiThresholds t;  // warn=-75
    const WpaEvent ev = makeEvent(WpaEventType::Connected, "eth0.100", -65);

    EXPECT_EQ(PathState::PathUp,
              PathStateFsm::nextState(PathState::Idle, ev, t));
    EXPECT_EQ(PathState::PathUp,
              PathStateFsm::nextState(PathState::PathDown, ev, t));
}

TEST(PathStateFsmNextState, ConnectedMediumRssi_GivesPathDegraded) {
    const RssiThresholds t;  // warn=-75, drop=-85
    const WpaEvent ev = makeEvent(WpaEventType::Connected, "eth0.100", -80);

    EXPECT_EQ(PathState::PathDegraded,
              PathStateFsm::nextState(PathState::Idle, ev, t));
    EXPECT_EQ(PathState::PathDegraded,
              PathStateFsm::nextState(PathState::PathDown, ev, t));
}

TEST(PathStateFsmNextState, ConnectedBadRssi_GivesPathDown) {
    const RssiThresholds t;  // drop=-85
    const WpaEvent ev = makeEvent(WpaEventType::Connected, "eth0.100", -90);

    EXPECT_EQ(PathState::PathDown,
              PathStateFsm::nextState(PathState::Idle, ev, t));
    EXPECT_EQ(PathState::PathDown,
              PathStateFsm::nextState(PathState::PathDown, ev, t));
}

TEST(PathStateFsmNextState, ConnectedFromPathUp_NoChange) {
    const RssiThresholds t;
    const WpaEvent ev = makeEvent(WpaEventType::Connected, "eth0.100", -65);

    // Already up — ignore repeated CONNECTED
    EXPECT_EQ(PathState::PathUp,
              PathStateFsm::nextState(PathState::PathUp, ev, t));
}

TEST(PathStateFsmNextState, SignalChangePathUp_WeakRssi_GivesPathDegraded) {
    const RssiThresholds t;  // warn=-75
    const WpaEvent ev = makeEvent(WpaEventType::SignalChange, "eth0.100", -80);

    EXPECT_EQ(PathState::PathDegraded,
              PathStateFsm::nextState(PathState::PathUp, ev, t));
}

TEST(PathStateFsmNextState, SignalChangePathUp_OkRssi_NoChange) {
    const RssiThresholds t;
    const WpaEvent ev = makeEvent(WpaEventType::SignalChange, "eth0.100", -60);

    EXPECT_EQ(PathState::PathUp,
              PathStateFsm::nextState(PathState::PathUp, ev, t));
}

TEST(PathStateFsmNextState, SignalChangePathDegraded_RecoverRssi_GivesPathUp) {
    const RssiThresholds t;  // warn=-75
    const WpaEvent ev = makeEvent(WpaEventType::SignalChange, "eth0.100", -70);

    EXPECT_EQ(PathState::PathUp,
              PathStateFsm::nextState(PathState::PathDegraded, ev, t));
}

TEST(PathStateFsmNextState, SignalChangePathDegraded_DroppedRssi_GivesPathDown) {
    const RssiThresholds t;  // drop=-85
    const WpaEvent ev = makeEvent(WpaEventType::SignalChange, "eth0.100", -90);

    EXPECT_EQ(PathState::PathDown,
              PathStateFsm::nextState(PathState::PathDegraded, ev, t));
}

TEST(PathStateFsmNextState, SignalChangePathDegraded_MediumRssi_StaysDegraded) {
    const RssiThresholds t;  // warn=-75, drop=-85
    const WpaEvent ev = makeEvent(WpaEventType::SignalChange, "eth0.100", -80);

    EXPECT_EQ(PathState::PathDegraded,
              PathStateFsm::nextState(PathState::PathDegraded, ev, t));
}

// ── PathStateFsm::onWpaEvent — callback firing tests ────────────────────────

TEST(PathStateFsmCallbacks, ConnectedGoodRssi_FiresOnPathUp) {
    bool pathUpCalled  = false;
    bool pathDownCalled = false;

    PathStateFsm::Callbacks cbs;
    cbs.onPathUp   = [&](std::string_view, uint32_t) { pathUpCalled = true; };
    cbs.onPathDown = [&](std::string_view, uint32_t) { pathDownCalled = true; };

    PathStateFsm fsm{makeConfig(), std::move(cbs)};
    ASSERT_EQ(fsm.currentState(), PathState::Idle);

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected, "eth0.100", -65));

    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    EXPECT_TRUE(pathUpCalled);
    EXPECT_FALSE(pathDownCalled);
}

TEST(PathStateFsmCallbacks, DisconnectedFromPathUp_FiresOnPathDown) {
    bool pathUpCalled  = false;
    bool pathDownCalled = false;

    PathStateFsm::Callbacks cbs;
    cbs.onPathUp   = [&](std::string_view, uint32_t) { pathUpCalled = true; };
    cbs.onPathDown = [&](std::string_view, uint32_t) { pathDownCalled = true; };

    PathStateFsm fsm{makeConfig(), std::move(cbs)};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected,     "eth0.100", -65));
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected,  "eth0.100"));

    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    EXPECT_TRUE(pathUpCalled);
    EXPECT_TRUE(pathDownCalled);
}

TEST(PathStateFsmCallbacks, PathUpThenDegraded_NoExtraCallbacks) {
    int pathUpCount   = 0;
    int pathDownCount = 0;

    PathStateFsm::Callbacks cbs;
    cbs.onPathUp   = [&](std::string_view, uint32_t) { ++pathUpCount; };
    cbs.onPathDown = [&](std::string_view, uint32_t) { ++pathDownCount; };

    PathStateFsm fsm{makeConfig(), std::move(cbs)};

    // Connect with good RSSI → PathUp
    fsm.onWpaEvent(makeEvent(WpaEventType::Connected, "eth0.100", -65));
    EXPECT_EQ(pathUpCount,   1);
    EXPECT_EQ(pathDownCount, 0);

    // Signal drops below warn → PathDegraded (route stays; no new callbacks)
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "eth0.100", -80));
    EXPECT_EQ(fsm.currentState(), PathState::PathDegraded);
    EXPECT_EQ(pathUpCount,   1);  // no NEW onPathUp
    EXPECT_EQ(pathDownCount, 0);  // no onPathDown yet

    // Signal recovers → PathUp (fires onPathUp again)
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "eth0.100", -65));
    EXPECT_EQ(fsm.currentState(), PathState::PathUp);
    EXPECT_EQ(pathUpCount,   2);
}

TEST(PathStateFsmCallbacks, PathDegradedThenDown_FiresOnPathDown) {
    bool pathDownCalled = false;

    PathStateFsm::Callbacks cbs;
    cbs.onPathUp   = [](std::string_view, uint32_t) {};
    cbs.onPathDown = [&](std::string_view, uint32_t) { pathDownCalled = true; };

    PathStateFsm fsm{makeConfig(), std::move(cbs)};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected,    "eth0.100", -65));
    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "eth0.100", -80));  // → Degraded
    EXPECT_FALSE(pathDownCalled);

    fsm.onWpaEvent(makeEvent(WpaEventType::SignalChange, "eth0.100", -92));  // → Down
    EXPECT_EQ(fsm.currentState(), PathState::PathDown);
    EXPECT_TRUE(pathDownCalled);
}

TEST(PathStateFsmCallbacks, TerminatingFromPathUp_FiresOnPathDown_ThenIdle) {
    bool pathDownCalled = false;
    PathState finalState = PathState::Idle;

    PathStateFsm::Callbacks cbs;
    cbs.onPathUp   = [](std::string_view, uint32_t) {};
    cbs.onPathDown = [&](std::string_view, uint32_t) { pathDownCalled = true; };
    cbs.onStateChanged = [&](PathState, PathState to, std::string_view, int) { finalState = to; };

    PathStateFsm fsm{makeConfig(), std::move(cbs)};
    fsm.onWpaEvent(makeEvent(WpaEventType::Connected,   "eth0.100", -65));
    fsm.onWpaEvent(makeEvent(WpaEventType::Terminating, "eth0.100"));

    // Terminating goes straight to Idle; route teardown is NOT called
    // (wpa_supplicant died — link is already down; kernel will flush routes).
    EXPECT_EQ(fsm.currentState(), PathState::Idle);
    EXPECT_EQ(finalState, PathState::Idle);
}

TEST(PathStateFsmCallbacks, onStateChangedFiredForEveryTransition) {
    std::vector<std::pair<PathState, PathState>> changes;

    PathStateFsm::Callbacks cbs;
    cbs.onPathUp       = [](std::string_view, uint32_t) {};
    cbs.onPathDown     = [](std::string_view, uint32_t) {};
    cbs.onStateChanged = [&](PathState from, PathState to, std::string_view, int) {
        changes.emplace_back(from, to);
    };

    PathStateFsm fsm{makeConfig(), std::move(cbs)};

    fsm.onWpaEvent(makeEvent(WpaEventType::Connected,    "eth0.100", -65));   // Idle→Up
    fsm.onWpaEvent(makeEvent(WpaEventType::Disconnected, "eth0.100"));         // Up→Down

    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[0].first,  PathState::Idle);
    EXPECT_EQ(changes[0].second, PathState::PathUp);
    EXPECT_EQ(changes[1].first,  PathState::PathUp);
    EXPECT_EQ(changes[1].second, PathState::PathDown);
}

