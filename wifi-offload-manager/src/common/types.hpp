#pragma once

// types.hpp — shared type definitions across all modules

#include <cstdint>
#include <string>
#include <vector>

namespace netservice {

// ── Path State FSM ────────────────────────────────────────────────
enum class PathState {
    Idle,
    Scanning,
    Connecting,
    PathUp,
    PathDegraded,
    PathDown,
};

[[nodiscard]] constexpr std::string_view toString(PathState s) noexcept {
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

// ── Path Events (consumer-facing) ─────────────────────────────────
enum class PathEvent {
    PathUp,
    PathDegraded,
    PathDown,
};

// ── RSSI thresholds (dBm) — populated from config or defaults ─────
struct RssiThresholds {
    int connectMin{-70};  // CONNECTING → PATH_UP
    int warn{-75};        // PATH_UP → PATH_DEGRADED
    int drop{-85};        // PATH_DEGRADED → PATH_DOWN
};

// ── Path Class Config (populated by ConfigLoader from JSON) ───────
struct PathClassConfig {
    std::string          id;             // e.g. "multipath"
    uint32_t             classid{};      // e.g. 0x00100001
    std::string          cgroupPath;     // e.g. "/sys/fs/cgroup/net_cls/multipath"
    std::vector<std::string> interfaces; // e.g. ["wlan0", "wwan0"]
    bool                 mptcpEnabled{false};
    uint32_t             routingTable{}; // e.g. 100
    uint32_t             mark{};         // e.g. 0x10
    bool                 strictIsolation{false};
    RssiThresholds       rssi;           // optional, applies to WiFi-capable classes
};

// ── Consumer API ─────────────────────────────────────────────────
struct PathInfo {
    PathEvent   event{};
    std::string iface;
    int         rssiDbm{0};
    uint32_t    estBandwidthKbps{0};
};

using PathHandle = uint64_t;
constexpr PathHandle kInvalidHandle = 0;

} // namespace netservice
