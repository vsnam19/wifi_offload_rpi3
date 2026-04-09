// test_routing_policy.cpp — unit tests for RoutingPolicyManager
//
// Tests cover behaviours that are observable without root/kernel privileges:
//   - queryGatewayForIface() static helper (reads /proc/net/route)
//   - addDefaultRoute() / removeDefaultRoute() with a non-existent interface
//   - createCgroupHierarchy() using /tmp paths (no sys/fs/cgroup needed)
//   - addIptablesMarkRules() / addIpRules() fail gracefully when unprivileged

#include "routing/routing_policy_manager.hpp"
#include "common/error.hpp"
#include "common/types.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace netservice;

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string makeTempCgroupBase(std::string_view prefix) {
    return std::string{"/tmp/"} + std::string{prefix} +
           std::to_string(static_cast<unsigned>(::getpid()));
}

static PathClassConfig makeClass(std::string id, uint32_t classId,
                                 std::string cgroupPath,
                                 uint32_t mark, uint32_t table) {
    PathClassConfig c;
    c.id           = std::move(id);
    c.classid      = classId;
    c.cgroupPath   = std::move(cgroupPath);
    c.interfaces   = {"eth0"};
    c.mptcpEnabled = false;
    c.routingTable = table;
    c.mark         = mark;
    c.strictIsolation = false;
    return c;
}

// ── queryGatewayForIface ──────────────────────────────────────────────────────

TEST(RoutingPolicyQueryGw, NonExistentIfaceReturnsZero) {
    // A made-up interface name will never appear in /proc/net/route.
    EXPECT_EQ(RoutingPolicyManager::queryGatewayForIface("__no_such_iface__"), 0u);
}

TEST(RoutingPolicyQueryGw, EmptyIfaceReturnsZero) {
    EXPECT_EQ(RoutingPolicyManager::queryGatewayForIface(""), 0u);
}

// ── addDefaultRoute / removeDefaultRoute ─────────────────────────────────────

TEST(RoutingPolicyRoutes, AddDefaultRouteUnknownIfaceFails) {
    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto r = mgr.addDefaultRoute("__no_such_iface__", 0, 100);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), RoutingError::NetlinkError);
}

TEST(RoutingPolicyRoutes, RemoveDefaultRouteUnknownIfaceSucceeds) {
    // Code treats an unknown interface as "already gone" → success.
    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto r = mgr.removeDefaultRoute("__no_such_iface__", 100);
    EXPECT_TRUE(r.has_value());
}

// ── createCgroupHierarchy — filesystem paths in /tmp ─────────────────────────

TEST(RoutingPolicyCgroup, CreateCgroupHierarchyUnderTmp) {
    const std::string baseDir    = makeTempCgroupBase("test_routing_cgroup_");
    const std::string cgroupPath = baseDir + "/test_class";

    // Pre-create the parent so canonical() resolves it.
    std::filesystem::create_directories(baseDir);

    std::vector<PathClassConfig> classes = {
        makeClass("test_class", 0x00100001u, cgroupPath, 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.createCgroupHierarchy();
    ASSERT_TRUE(result.has_value()) << "createCgroupHierarchy failed";

    // Verify the cgroup directory exists.
    EXPECT_TRUE(std::filesystem::exists(cgroupPath));

    // Verify net_cls.classid was written.
    const std::string classidFile = cgroupPath + "/net_cls.classid";
    ASSERT_TRUE(std::filesystem::exists(classidFile));

    std::ifstream f{classidFile};
    std::string content;
    std::getline(f, content);
    EXPECT_EQ(content, "0x00100001");

    // Cleanup.
    std::filesystem::remove_all(baseDir);
}

TEST(RoutingPolicyCgroup, CreateCgroupHierarchyIdempotent) {
    const std::string baseDir    = makeTempCgroupBase("test_routing_cgroup_idem_");
    const std::string cgroupPath = baseDir + "/cls_a";

    std::filesystem::create_directories(baseDir);

    std::vector<PathClassConfig> classes = {
        makeClass("cls_a", 0x00200002u, cgroupPath, 0x20u, 200)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    // Call twice — should succeed both times (idempotent).
    EXPECT_TRUE(mgr.createCgroupHierarchy().has_value());
    EXPECT_TRUE(mgr.createCgroupHierarchy().has_value());

    std::filesystem::remove_all(baseDir);
}

TEST(RoutingPolicyCgroup, CreateCgroupHierarchyBadParentFails) {
    // Use a path whose parent does not exist, so canonical() will fail.
    const std::string pathWithMissingParent =
        makeTempCgroupBase("test_routing_bad_parent_") + "/no_such_subdir";
    // Ensure the path is truly absent.
    std::filesystem::remove_all(pathWithMissingParent);

    std::vector<PathClassConfig> classes = {
        makeClass("bad", 0x00100001u,
                  pathWithMissingParent + "/cls",
                  0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.createCgroupHierarchy();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::CgroupCreateFailed);
}

// ── addIptablesMarkRules / addIpRules — unprivileged failure ─────────────────

TEST(RoutingPolicyUnprivileged, AddIptablesMarkRulesFailsWithoutRoot) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "skipped: running as root";
    }

    std::vector<PathClassConfig> classes = {
        makeClass("cls", 0x00100001u, "/sys/fs/cgroup/net_cls/cls", 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    // iptc_init() fails without CAP_NET_ADMIN → IptablesError expected.
    auto result = mgr.addIptablesMarkRules();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::IptablesError);
}

TEST(RoutingPolicyUnprivileged, AddIpRulesFailsWithoutRoot) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "skipped: running as root";
    }

    std::vector<PathClassConfig> classes = {
        makeClass("cls", 0x00100001u, "/sys/fs/cgroup/net_cls/cls", 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.addIpRules();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::NetlinkError);
}

