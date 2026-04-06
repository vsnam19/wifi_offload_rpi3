// routing_policy_manager.cpp — P2-T1: cgroup net_cls hierarchy setup

#include "routing/routing_policy_manager.hpp"
#include "common/logger.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>

namespace netservice {

namespace {

// Root of the cgroup net_cls hierarchy.
// With cgroup v1 (systemd.unified_cgroup_hierarchy=0), this is always
// /sys/fs/cgroup/net_cls/.  The path is read from PathClassConfig::cgroupPath
// per class, so this constant is only used for log messages.
constexpr std::string_view kCgroupNetClsRoot = "/sys/fs/cgroup/net_cls";

} // namespace

RoutingPolicyManager::RoutingPolicyManager(
    std::span<const PathClassConfig> classes) noexcept
    : classes_{classes}
{}

std::expected<void, RoutingError> RoutingPolicyManager::createCgroupHierarchy()
{
    logger::info("[ROUTING] creating cgroup net_cls hierarchy ({} class(es))",
                 classes_.size());

    for (const auto& cls : classes_) {
        if (auto result = setupOneClass(cls); !result) {
            return result; // propagate first error
        }
    }

    logger::info("[ROUTING] cgroup hierarchy ready");
    return {};
}

std::expected<void, RoutingError>
RoutingPolicyManager::setupOneClass(const PathClassConfig& cls)
{
    const std::filesystem::path cgroupDir{cls.cgroupPath};

    // ── 0. Resolve symlinks in the parent path ─────────────────────
    // /sys/fs/cgroup/net_cls is a symlink to net_cls,net_prio on cgroup v1.
    // ReadWritePaths in systemd does NOT follow symlinks when binding mounts,
    // so we must use the canonical (real) path for filesystem operations.
    std::error_code ec;
    const auto realParent = std::filesystem::canonical(cgroupDir.parent_path(), ec);
    if (ec) {
        logger::error("[ROUTING] cannot resolve cgroup parent: path={} err={} ({})",
                      cgroupDir.parent_path().string(), ec.value(), ec.message());
        return std::unexpected(RoutingError::CgroupCreateFailed);
    }
    const auto realCgroupDir = realParent / cgroupDir.filename();

    // ── 1. Create directory ────────────────────────────────────────
    std::filesystem::create_directories(realCgroupDir, ec);
    if (ec) {
        logger::error("[ROUTING] mkdir failed: path={} err={} ({})",
                      realCgroupDir.string(), ec.value(), ec.message());
        return std::unexpected(RoutingError::CgroupCreateFailed);
    }

    // ── 2. Write classid ───────────────────────────────────────────
    // Format: upper 16 bits : lower 16 bits in decimal, e.g. "1:1"
    // The kernel net_cls.classid file accepts a hex value like "0x00100001".
    const std::filesystem::path classidFile = realCgroupDir / "net_cls.classid";

    // Read existing value to determine if a write is needed.
    uint32_t existing{};
    {
        std::ifstream ifs{classidFile};
        if (ifs.good()) {
            ifs >> std::hex >> existing;
        }
    }

    if (existing != cls.classid) {
        std::ofstream ofs{classidFile};
        if (!ofs.is_open()) {
            logger::error("[ROUTING] cannot open classid file: path={} errno={} ({})",
                          classidFile.string(), errno, std::strerror(errno));
            return std::unexpected(RoutingError::CgroupWriteFailed);
        }
        ofs << std::format("0x{:08x}", cls.classid) << '\n';
        if (ofs.fail()) {
            logger::error("[ROUTING] write classid failed: path={} classid=0x{:08x}",
                          classidFile.string(), cls.classid);
            return std::unexpected(RoutingError::CgroupWriteFailed);
        }
        logger::info("[ROUTING] cgroup created: id={} real_path={} classid=0x{:08x}",
                     cls.id, realCgroupDir.string(), cls.classid);
    } else {
        logger::info("[ROUTING] cgroup exists (unchanged): id={} classid=0x{:08x}",
                     cls.id, cls.classid);
    }

    return {};
}

void RoutingPolicyManager::cleanup() noexcept
{
    // Remove cgroup directories in reverse order (children before parents).
    // Directories can only be removed when they have no tasks; the daemon
    // is the one that placed processes in them (Phase 5), so by shutdown time
    // they should be empty.
    for (auto it = classes_.rbegin(); it != classes_.rend(); ++it) {
        const std::filesystem::path cgroupDir{it->cgroupPath};
        std::error_code ec;
        // Resolve symlinks so we operate on the real mountpoint
        const auto realParent = std::filesystem::canonical(cgroupDir.parent_path(), ec);
        if (ec) {
            logger::warn("[ROUTING] cleanup: cannot resolve parent for {}: {}",
                         it->cgroupPath, ec.message());
            continue;
        }
        const auto realCgroupDir = realParent / cgroupDir.filename();
        if (std::filesystem::exists(realCgroupDir, ec) && !ec) {
            std::filesystem::remove(realCgroupDir, ec);
            if (ec) {
                logger::warn("[ROUTING] cleanup: failed to remove {}: {}",
                             realCgroupDir.string(), ec.message());
            } else {
                logger::info("[ROUTING] cleanup: removed cgroup {}", realCgroupDir.string());
            }
        }
    }
}

} // namespace netservice
