#pragma once

// routing_policy_manager.hpp — cgroup net_cls + iptables + ip rule setup
//
// Phase 2 scope:
//   P2-T1  createCgroupHierarchy()  — mkdir + write net_cls.classid per class
//   P2-T2  addIptablesMarkRules()   — iptables mangle OUTPUT: classid→fwmark
//   P2-T3  addIpRules()             — ip rule: fwmark → routing table (Netlink)
//   P2-T4  addDropRules()           — iptables filter DROP: strict_isolation safety net
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

    // ── P2-T2 ─────────────────────────────────────────────────────
    // In the mangle table, OUTPUT chain, install:
    //   -m cgroup --cgroup <classid> -j MARK --set-xmark <mark>/0xffffffff
    // for every configured path class.
    //
    // Rules are installed into a dedicated user chain NETSERVICE-MARK to
    // allow atomic flush+recreate on daemon restart (idempotent).
    [[nodiscard]] std::expected<void, RoutingError> addIptablesMarkRules();

    // ── P2-T3 ─────────────────────────────────────────────────────
    // For each path class, add an ip rule:
    //   ip rule add fwmark <mark> lookup <routing_table>
    // Implemented via Netlink RTM_NEWRULE (libmnl). Idempotent (EEXIST → ok).
    [[nodiscard]] std::expected<void, RoutingError> addIpRules();

    // ── P2-T4 ─────────────────────────────────────────────────────
    // For each class with strict_isolation=true (e.g. lte_b2b), install
    // safety-net DROP rules in the filter table OUTPUT chain:
    //   (a) class's fwmark must not exit on any interface NOT in its own list
    //   (b) every other class's fwmark must not exit on the isolation class's
    //       interfaces
    // Rules are collected in a dedicated NETSERVICE-ISO user chain (idempotent).
    [[nodiscard]] std::expected<void, RoutingError> addDropRules();

    // ── P2-T5 (partial) ───────────────────────────────────────────
    // Remove all kernel state created by this manager:
    //   - ip rules (P2-T3)
    //   - iptables DROP rules in NETSERVICE-ISO chain (P2-T4)
    //   - iptables MARK rules in NETSERVICE-MARK chain (P2-T2)
    //   - cgroup directories (P2-T1)
    // Called on clean daemon shutdown.
    void cleanup() noexcept;

    // ── P4-T3 / P4-T4 — dynamic route management ─────────────────
    // Add a default (0.0.0.0/0) route to `routingTable` via `gwAddr`
    // on interface `iface`.  gwAddr must be in network byte order.
    // Idempotent (EEXIST treated as success).
    [[nodiscard]] std::expected<void, RoutingError>
    addDefaultRoute(std::string_view iface, uint32_t gwAddr,
                    uint32_t routingTable) noexcept;

    // Remove the default route from `routingTable` via `iface`.
    // Idempotent (ENOENT treated as success).
    [[nodiscard]] std::expected<void, RoutingError>
    removeDefaultRoute(std::string_view iface, uint32_t routingTable) noexcept;

    // Query the current default gateway for `iface` from /proc/net/route.
    // Returns the gateway in network byte order, or 0 if not found.
    [[nodiscard]] static uint32_t
    queryGatewayForIface(std::string_view iface) noexcept;

private:
    std::span<const PathClassConfig> classes_; // non-owning view into config

    // Create one cgroup dir and write its classid.
    [[nodiscard]] std::expected<void, RoutingError>
    setupOneClass(const PathClassConfig& cls);

    // Remove iptables rules managed by this instance (called from cleanup).
    void removeIptablesMarkRules() noexcept;

    // Remove ip rules added by addIpRules() (called from cleanup).
    void removeIpRules() noexcept;

    // Remove DROP rules added by addDropRules() (called from cleanup).
    void removeDropRules() noexcept;
};

} // namespace netservice
