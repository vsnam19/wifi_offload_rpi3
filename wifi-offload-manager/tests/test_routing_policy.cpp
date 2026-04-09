// test_routing_policy.cpp — unit tests for RoutingPolicyManager
//
// Tests target 100% line/branch coverage for all public methods of
// RoutingPolicyManager that are observable without root/kernel privileges.
// Branches requiring CAP_NET_ADMIN (iptables commit, RTM_NEWRULE success)
// are guarded by GTEST_SKIP() when running as root, or documented as
// unreachable without elevated privileges.

#include "routing/routing_policy_manager.hpp"
#include "common/error.hpp"
#include "common/types.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
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
    c.id              = std::move(id);
    c.classid         = classId;
    c.cgroupPath      = std::move(cgroupPath);
    c.interfaces      = {"eth0"};
    c.mptcpEnabled    = false;
    c.routingTable    = table;
    c.mark            = mark;
    c.strictIsolation = false;
    return c;
}

// Return the first interface name found in /proc/net/route that has a default
// route (Destination == "00000000") with a non-zero gateway. Returns "" if
// none is found.
static std::string findIfaceWithDefaultRoute() {
    std::ifstream f{"/proc/net/route"};
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line); // skip header
    while (std::getline(f, line)) {
        std::istringstream ss{line};
        std::string dev, dst, gw;
        if (!(ss >> dev >> dst >> gw)) continue;
        if (dst == "00000000" && gw != "00000000") return dev;
    }
    return {};
}

// ── queryGatewayForIface ──────────────────────────────────────────────────────

TEST(RoutingPolicyQueryGw, NonExistentIfaceReturnsZero) {
    // A made-up interface name will never appear in /proc/net/route.
    EXPECT_EQ(RoutingPolicyManager::queryGatewayForIface("__no_such_iface__"), 0u);
}

TEST(RoutingPolicyQueryGw, EmptyIfaceReturnsZero) {
    EXPECT_EQ(RoutingPolicyManager::queryGatewayForIface(""), 0u);
}

TEST(RoutingPolicyQueryGw, FindsGatewayForIfaceWithDefaultRoute) {
    // Cover the "found" branch: iface matches and dst == "00000000".
    const auto iface = findIfaceWithDefaultRoute();
    if (iface.empty()) {
        GTEST_SKIP() << "no interface with a non-zero default gateway in /proc/net/route";
    }
    const uint32_t gw = RoutingPolicyManager::queryGatewayForIface(iface);
    EXPECT_NE(gw, 0u) << "expected non-zero gateway for iface=" << iface;
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

TEST(RoutingPolicyRoutes, AddDefaultRouteValidIfaceNetlinkFails) {
    // "lo" always has a valid ifindex. Without CAP_NET_ADMIN the kernel
    // rejects RTM_NEWROUTE with EPERM → NetlinkError.
    if (::geteuid() == 0) {
        GTEST_SKIP() << "skipped: running as root";
    }

    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto r = mgr.addDefaultRoute("lo", 0, 100);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), RoutingError::NetlinkError);
}

TEST(RoutingPolicyRoutes, AddDefaultRouteWithGatewayNetlinkFails) {
    // Non-zero gwAddr exercises the RTA_GATEWAY attribute branch inside
    // sendRouteMsg (line 773).  Without root the kernel still rejects it.
    if (::geteuid() == 0) {
        GTEST_SKIP() << "skipped: running as root";
    }

    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    // 192.168.1.1 in network byte order
    constexpr uint32_t kGw = 0xC0A80101u;
    auto r = mgr.addDefaultRoute("lo", kGw, 100);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), RoutingError::NetlinkError);
}

TEST(RoutingPolicyRoutes, RemoveDefaultRouteValidIfaceDoesNotCrash) {
    // "lo" has a valid ifindex so sendRouteMsg is actually invoked (lines
    // 844-846).  Without root the kernel returns EPERM; the result is either
    // NetlinkError or success (if the kernel treats absent rule as ENOENT).
    // Either outcome is valid; the important invariant is no crash.
    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    (void)mgr.removeDefaultRoute("lo", 100);
    SUCCEED();
}

TEST(RoutingPolicyRoutes, AddIpRulesEmptyClassesSucceeds) {
    // With zero classes the per-class loop body is skipped and the function
    // returns the "ready" success path (line 450-451 in the source).
    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.addIpRules();
    EXPECT_TRUE(result.has_value())
        << "addIpRules() with no classes must return success";
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

TEST(RoutingPolicyCgroup, CreateDirectoryFails) {
    // Place a regular FILE at the path that the code would treat as the
    // cgroup parent dir.  canonical() resolves it (file exists) but
    // create_directories(file/cls) fails because "file" is not a dir.
    const std::string filePath = makeTempCgroupBase("test_routing_file_at_parent_");
    { std::ofstream ofs{filePath}; ASSERT_TRUE(ofs.is_open()); }

    std::vector<PathClassConfig> classes = {
        makeClass("blocked", 0x00100001u, filePath + "/cls", 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.createCgroupHierarchy();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::CgroupCreateFailed);

    std::filesystem::remove(filePath);
}

TEST(RoutingPolicyCgroup, WriteClassidFileFails) {
    // Cgroup dir exists; classid file exists but is mode 000 so ofstream
    // cannot open it for writing → CgroupWriteFailed.
    const std::string baseDir     = makeTempCgroupBase("test_routing_write_fail_");
    const std::string cgroupPath  = baseDir + "/cls_ro";
    const std::string classidFile = cgroupPath + "/net_cls.classid";

    std::filesystem::create_directories(cgroupPath);
    { std::ofstream ofs{classidFile}; } // touch the file
    std::filesystem::permissions(classidFile,
        std::filesystem::perms::none,
        std::filesystem::perm_options::replace);

    // cls.classid = 0x00100001; existing is unreadable → treated as 0.
    // 0 != 0x00100001 → write attempted → fails → CgroupWriteFailed.
    std::vector<PathClassConfig> classes = {
        makeClass("cls_ro", 0x00100001u, cgroupPath, 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.createCgroupHierarchy();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::CgroupWriteFailed);

    // Restore write permission before cleanup.
    std::filesystem::permissions(classidFile,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    std::filesystem::remove_all(baseDir);
}

TEST(RoutingPolicyCgroup, StopsAtFirstErrorInMultiClass) {
    // First class has a non-resolvable parent → CgroupCreateFailed.
    // Second class has a valid parent. Verify iteration stops immediately
    // and the second class directory is never created.
    const std::string baseDir = makeTempCgroupBase("test_routing_multi_fail_");
    std::filesystem::create_directories(baseDir);
    const std::string goodPath  = baseDir + "/good_cls";

    const std::string badParent =
        makeTempCgroupBase("test_routing_multi_bad_") + "/no_such_dir";
    std::filesystem::remove_all(badParent);

    std::vector<PathClassConfig> classes = {
        makeClass("bad_first",   0x00100001u, badParent + "/cls", 0x10u, 100),
        makeClass("good_second", 0x00200002u, goodPath,           0x20u, 200),
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.createCgroupHierarchy();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::CgroupCreateFailed);

    // good_second must NOT have been created (iteration stopped at first error).
    EXPECT_FALSE(std::filesystem::exists(goodPath));

    std::filesystem::remove_all(baseDir);
}

// ── addDropRules ──────────────────────────────────────────────────────────────

TEST(RoutingPolicyDropRules, NoIsolationReturnsSuccess) {
    // When no class has strict_isolation, addDropRules() short-circuits and
    // returns success without touching iptables (no root required).
    std::vector<PathClassConfig> classes = {
        makeClass("cls_a", 0x00100001u, "/tmp/cls_a", 0x10u, 100),
        makeClass("cls_b", 0x00200002u, "/tmp/cls_b", 0x20u, 200),
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.addDropRules();
    EXPECT_TRUE(result.has_value())
        << "addDropRules() with no strict_isolation classes must succeed";
}

TEST(RoutingPolicyDropRules, EmptyClassesNoIsolationReturnsSuccess) {
    std::vector<PathClassConfig> classes;
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.addDropRules();
    EXPECT_TRUE(result.has_value());
}

TEST(RoutingPolicyDropRules, WithIsolationFailsWithoutRoot) {
    // With strict_isolation=true, iptc_init(filter) is called.
    // Without CAP_NET_ADMIN it fails → IptablesError.
    if (::geteuid() == 0) {
        GTEST_SKIP() << "skipped: running as root";
    }

    PathClassConfig iso = makeClass("isolated", 0x00300003u, "/tmp/iso", 0x30u, 300);
    iso.strictIsolation = true;
    std::vector<PathClassConfig> classes = { iso };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    auto result = mgr.addDropRules();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RoutingError::IptablesError);
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

// ── cleanup() ────────────────────────────────────────────────────────────────

TEST(RoutingPolicyCleanup, RemovesEmptyCgroupDir) {
    // cleanup() must remove a cgroup dir that has no files inside it.
    const std::string baseDir    = makeTempCgroupBase("test_routing_cleanup_ok_");
    const std::string cgroupPath = baseDir + "/cleanup_cls";
    std::filesystem::create_directories(cgroupPath); // empty dir

    std::vector<PathClassConfig> classes = {
        makeClass("cleanup_cls", 0x00100001u, cgroupPath, 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    mgr.cleanup(); // noexcept — must not abort

    EXPECT_FALSE(std::filesystem::exists(cgroupPath))
        << "cleanup() should have removed the empty cgroup directory";

    std::filesystem::remove_all(baseDir);
}

TEST(RoutingPolicyCleanup, NonEmptyDirRemoveFailsNocrash) {
    // std::filesystem::remove() fails on non-empty dirs; cleanup() must log a
    // warning and continue without aborting.
    const std::string baseDir    = makeTempCgroupBase("test_routing_cleanup_nonempty_");
    const std::string cgroupPath = baseDir + "/cleanup_nonempty";
    std::filesystem::create_directories(cgroupPath);
    { std::ofstream{cgroupPath + "/net_cls.classid"} << "0x00100001\n"; }

    std::vector<PathClassConfig> classes = {
        makeClass("cleanup_nonempty", 0x00100001u, cgroupPath, 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    mgr.cleanup(); // must not crash despite remove() failure on non-empty dir

    // Dir should still exist (remove failed).
    EXPECT_TRUE(std::filesystem::exists(cgroupPath));

    std::filesystem::remove_all(baseDir);
}

TEST(RoutingPolicyCleanup, SkipsBadParentGracefully) {
    // canonical(parent_path) fails → cleanup() logs a warning and continues.
    const std::string pathWithMissingParent =
        makeTempCgroupBase("test_routing_cleanup_bad_") + "/no_such_dir";
    std::filesystem::remove_all(pathWithMissingParent);

    std::vector<PathClassConfig> classes = {
        makeClass("bad", 0x00100001u, pathWithMissingParent + "/cls", 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    mgr.cleanup(); // must not crash
    SUCCEED(); // reaching here means no crash/abort
}

TEST(RoutingPolicyCleanup, MultipleClassesCleanedInReverseOrder) {
    // cleanup() iterates classes_ in reverse; verify both empty dirs are removed.
    const std::string baseDir = makeTempCgroupBase("test_routing_cleanup_multi_");
    const std::string pathA   = baseDir + "/cls_a";
    const std::string pathB   = baseDir + "/cls_b";
    std::filesystem::create_directories(pathA);
    std::filesystem::create_directories(pathB);

    std::vector<PathClassConfig> classes = {
        makeClass("cls_a", 0x00100001u, pathA, 0x10u, 100),
        makeClass("cls_b", 0x00200002u, pathB, 0x20u, 200),
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    mgr.cleanup();

    EXPECT_FALSE(std::filesystem::exists(pathA));
    EXPECT_FALSE(std::filesystem::exists(pathB));

    std::filesystem::remove_all(baseDir);
}

TEST(RoutingPolicyCleanup, NonExistentCgroupDirSkippedSilently) {
    // If the cgroup dir does not exist, cleanup() must silently skip it
    // (std::filesystem::exists returns false → no remove attempt).
    const std::string baseDir    = makeTempCgroupBase("test_routing_cleanup_nodir_");
    const std::string cgroupPath = baseDir + "/nonexistent_cls";
    std::filesystem::create_directories(baseDir); // parent exists; child does not

    std::vector<PathClassConfig> classes = {
        makeClass("nonexistent_cls", 0x00100001u, cgroupPath, 0x10u, 100)
    };
    RoutingPolicyManager mgr{std::span<const PathClassConfig>{classes}};

    mgr.cleanup(); // must not crash; dir was never created
    SUCCEED();

    std::filesystem::remove_all(baseDir);
}

