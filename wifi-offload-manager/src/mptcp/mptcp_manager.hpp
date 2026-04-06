#pragma once

// mptcp_manager.hpp — MPTCP endpoint lifecycle management
//
//   Registers / removes MPTCP PM endpoints for classes with mptcp_enabled=true.
//   Uses Generic Netlink (mptcp_pm family) via libmnl.
//
//   Implemented tasks:
//     Phase 4 — MPTCP endpoint add / remove (brought forward — required for VLAN test)

#include "common/error.hpp"
#include "common/types.hpp"

#include <expected>
#include <vector>

namespace netservice {

class MptcpManager {
public:
    explicit MptcpManager(std::vector<PathClassConfig> classes);

    // Add MPTCP endpoints for all classes with mptcp_enabled=true.
    // Each interface in the class gets one endpoint (subflow flag).
    // Idempotent: existing endpoints are skipped (EEXIST treated as success).
    [[nodiscard]] std::expected<void, RoutingError> addEndpoints();

    // Remove all MPTCP endpoints that were previously added by addEndpoints().
    // ENOENT treated as success (already removed).
    void removeEndpoints() noexcept;

    // ── P4-T3 / P4-T4 ─────────────────────────────────────────────
    // Per-interface endpoint management — called by PathStateFsm via main.cpp.
    // Only meaningful for classes with mptcp_enabled: true.
    //
    // addEndpointForIface: add MPTCP subflow endpoint for <iface>.
    //   No-op (returns success) if the interface has no IPv4 address yet.
    //
    // removeEndpointForIface: remove MPTCP subflow endpoint for <iface>.
    //   ENOENT (endpoint already absent) is treated as success.
    [[nodiscard]] std::expected<void, RoutingError>
    addEndpointForIface(std::string_view iface);

    void removeEndpointForIface(std::string_view iface) noexcept;

private:
    std::vector<PathClassConfig> classes_;

    // Track endpoint IDs assigned by kernel so we can remove them later.
    // Kernel assigns ID 1..N sequentially; we flush all rather than track IDs.

    [[nodiscard]] std::expected<void, RoutingError>
    sendEndpointCmd(int genlFamilyId, uint8_t cmd,
                    const char* iface, uint32_t addr4) noexcept;

    [[nodiscard]] std::expected<int, RoutingError>
    resolveGenlFamily() noexcept;
};

} // namespace netservice
