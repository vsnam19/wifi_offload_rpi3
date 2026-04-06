#pragma once

// routing_policy_manager.hpp — cgroup net_cls + iptables + ip rule setup
//
// Phase 2 scope:
//   P2-T1  createCgroupHierarchy()  — mkdir + write net_cls.classid per class
//   P2-T2  (next) addIptablesRules()
//   P2-T3  (next) addIpRules()
//   P2-T4  (next) addDropRules() for strict_isolation classes
//   P2-T5  (next) cleanup()
//
// Module boundary: does NOT know consumer PIDs, does NOT add routes to
// routing tables (Phase 4). Only sets up the kernel policy skeleton.

#include "common/error.hpp"
#include "common/types.hpp"

#include <expected>
#include <span>

namespace netservice {

class RoutingPolicyManager {
public:
    // Construct with the loaded path class configs.
    explicit RoutingPolicyManager(std::span<const PathClassConfig> classes) noexcept;

    // ── P2-T1 ─────────────────────────────────────────────────────
    // Create /sys/fs/cgroup/net_cls/<id>/ and write net_cls.classid for
    // every configured path class.
    //
    // Idempotent: existing directories are left unchanged; existing classid
    // files are overwritten only if the value differs.
    //
    // Returns the first RoutingError encountered, or void on success.
    [[nodiscard]] std::expected<void, RoutingError> createCgroupHierarchy();

    // ── P2-T5 (stub) ──────────────────────────────────────────────
    // Remove all cgroup directories created by this manager.
    // Called on clean daemon shutdown.
    void cleanup() noexcept;

private:
    std::span<const PathClassConfig> classes_; // non-owning view into config

    // Create one cgroup dir and write its classid.
    [[nodiscard]] std::expected<void, RoutingError>
    setupOneClass(const PathClassConfig& cls);
};

} // namespace netservice
