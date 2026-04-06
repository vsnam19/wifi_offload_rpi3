// routing_policy_manager.cpp — P2-T1/P2-T2: cgroup net_cls hierarchy + iptables MARK rules

#include "routing/routing_policy_manager.hpp"
#include "common/logger.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

// libiptc and kernel netfilter headers for P2-T2
#include <libiptc/libiptc.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_cgroup.h>
#include <linux/netfilter/xt_mark.h>

namespace netservice {

namespace {

// Root of the cgroup net_cls hierarchy.
// With cgroup v1 (systemd.unified_cgroup_hierarchy=0), this is always
// /sys/fs/cgroup/net_cls/.  The path is read from PathClassConfig::cgroupPath
// per class, so this constant is only used for log messages.
constexpr std::string_view kCgroupNetClsRoot = "/sys/fs/cgroup/net_cls";

// ── P2-T2 constants ───────────────────────────────────────────────
constexpr const char* kMangleTable     = "mangle";
constexpr const char* kOutputChain     = "OUTPUT";
constexpr const char* kNetserviceChain = "NETSERVICE-MARK";

// Align to XT_ALIGN boundary (same as XT_ALIGN macro in x_tables.h).
// On ARM EABI, alignof(_xt_align) = 8 (due to __u64 member).
constexpr size_t xtAlign(size_t n) noexcept {
    constexpr size_t kAlign = alignof(_xt_align);
    return (n + kAlign - 1u) & ~(kAlign - 1u);
}

// Build a heap buffer containing an ipt_entry for:
//   -t mangle -m cgroup --cgroup <classid> -j MARK --set-xmark <mark>/0xffffffff
//
// The iptables userspace queries the kernel for the highest supported cgroup
// match revision.  On kernel 6.6+, that is revision 2 (xt_cgroup_info_v2,
// which covers both --path and --cgroup classid via a union).  We use v2 to
// match what iptables userspace produces so the rules display correctly.
std::vector<uint8_t> buildMarkEntry(uint32_t classid, uint32_t mark)
{
    constexpr size_t kMatchSz  = xtAlign(sizeof(xt_entry_match) + sizeof(xt_cgroup_info_v2));
    constexpr size_t kTargetSz = xtAlign(sizeof(xt_entry_target) + sizeof(xt_mark_tginfo2));
    constexpr size_t kHdrSz    = xtAlign(sizeof(ipt_entry));
    constexpr size_t kTotal    = kHdrSz + kMatchSz + kTargetSz;

    std::vector<uint8_t> buf(kTotal, 0);

    auto* e  = reinterpret_cast<ipt_entry*>     (buf.data());
    auto* m  = reinterpret_cast<xt_entry_match*>(buf.data() + kHdrSz);
    auto* cg = reinterpret_cast<xt_cgroup_info_v2*>(m->data);
    auto* t  = reinterpret_cast<xt_entry_target*>(buf.data() + kHdrSz + kMatchSz);
    auto* mk = reinterpret_cast<xt_mark_tginfo2*>(t->data);

    // Match: "cgroup" rev 2 (xt_cgroup_info_v2 — classid-based)
    m->u.user.match_size = static_cast<uint16_t>(kMatchSz);
    std::strncpy(m->u.user.name, "cgroup", XT_EXTENSION_MAXNAMELEN - 1);
    m->u.user.revision   = 2;
    cg->has_classid      = 1;
    cg->has_path         = 0;
    cg->invert_classid   = 0;
    cg->invert_path      = 0;
    cg->classid          = classid;  // union with path[], classid is the 32-bit member

    // Target: "MARK" rev 2 (xt_mark_tginfo2: mark + mask)
    t->u.user.target_size = static_cast<uint16_t>(kTargetSz);
    std::strncpy(t->u.user.name, "MARK", XT_EXTENSION_MAXNAMELEN - 1);
    t->u.user.revision    = 2;
    mk->mark              = mark;
    mk->mask              = 0xFFFFFFFFu;

    // ipt_entry offsets
    e->target_offset = static_cast<uint16_t>(kHdrSz + kMatchSz);
    e->next_offset   = static_cast<uint16_t>(kTotal);

    return buf;
}

// Build a heap buffer for a jump rule: -A <fromChain> -j <toChain>
std::vector<uint8_t> buildJumpEntry(const char* toChain)
{
    constexpr size_t kTargetSz = xtAlign(sizeof(xt_standard_target));
    constexpr size_t kHdrSz    = xtAlign(sizeof(ipt_entry));
    constexpr size_t kTotal    = kHdrSz + kTargetSz;

    std::vector<uint8_t> buf(kTotal, 0);

    auto* e = reinterpret_cast<ipt_entry*>(buf.data());
    auto* t = reinterpret_cast<xt_standard_target*>(buf.data() + kHdrSz);

    t->target.u.user.target_size = static_cast<uint16_t>(kTargetSz);
    std::strncpy(t->target.u.user.name, toChain, XT_EXTENSION_MAXNAMELEN - 1);
    // verdict is resolved by libiptc at commit time

    e->target_offset = static_cast<uint16_t>(kHdrSz);
    e->next_offset   = static_cast<uint16_t>(kTotal);

    return buf;
}

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

// ── P2-T2 ─────────────────────────────────────────────────────────

std::expected<void, RoutingError> RoutingPolicyManager::addIptablesMarkRules()
{
    logger::info("[ROUTING] setting up iptables mangle MARK rules ({} class(es))",
                 classes_.size());

    // ── 1. Open mangle table ──────────────────────────────────────
    struct xtc_handle* raw = iptc_init(kMangleTable);
    if (!raw) {
        logger::error("[ROUTING] iptc_init(mangle) failed: {}", iptc_strerror(errno));
        return std::unexpected(RoutingError::IptablesError);
    }
    auto h = std::unique_ptr<xtc_handle, decltype(&iptc_free)>{raw, iptc_free};

    // ── 2. Create (or flush) the NETSERVICE-MARK user chain ───────
    // Using a dedicated chain allows atomic flush+recreate on daemon restart,
    // making the rule installation fully idempotent.
    if (iptc_is_chain(kNetserviceChain, h.get())) {
        if (!iptc_flush_entries(kNetserviceChain, h.get())) {
            logger::error("[ROUTING] flush chain {} failed: {}",
                          kNetserviceChain, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
        logger::info("[ROUTING] flushed existing chain {}", kNetserviceChain);
    } else {
        if (!iptc_create_chain(kNetserviceChain, h.get())) {
            logger::error("[ROUTING] create chain {} failed: {}",
                          kNetserviceChain, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
    }

    // ── 3. Ensure OUTPUT → NETSERVICE-MARK jump exists ───────────
    // Iterate the OUTPUT chain to avoid duplicate jump rules on restart.
    bool jumpExists = false;
    for (const ipt_entry* e = iptc_first_rule(kOutputChain, h.get());
         e != nullptr;
         e = iptc_next_rule(e, h.get())) {
        if (std::string_view{iptc_get_target(e, h.get())} == kNetserviceChain) {
            jumpExists = true;
            break;
        }
    }
    if (!jumpExists) {
        auto jumpBuf = buildJumpEntry(kNetserviceChain);
        const auto* je = reinterpret_cast<const ipt_entry*>(jumpBuf.data());
        if (!iptc_append_entry(kOutputChain, je, h.get())) {
            logger::error("[ROUTING] append OUTPUT→{} jump failed: {}",
                          kNetserviceChain, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
        logger::info("[ROUTING] added OUTPUT jump to {}", kNetserviceChain);
    } else {
        logger::info("[ROUTING] OUTPUT jump to {} already present", kNetserviceChain);
    }

    // ── 4. Add classid → mark rules to NETSERVICE-MARK ───────────
    for (const auto& cls : classes_) {
        auto ruleBuf = buildMarkEntry(cls.classid, cls.mark);
        const auto* re = reinterpret_cast<const ipt_entry*>(ruleBuf.data());
        if (!iptc_append_entry(kNetserviceChain, re, h.get())) {
            logger::error("[ROUTING] append MARK rule failed: id={} classid=0x{:08x}: {}",
                          cls.id, cls.classid, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
        logger::info("[ROUTING] iptables rule added: id={} classid=0x{:08x} → mark=0x{:x}",
                     cls.id, cls.classid, cls.mark);
    }

    // ── 5. Commit all changes atomically ─────────────────────────
    if (!iptc_commit(h.get())) {
        logger::error("[ROUTING] iptc_commit failed: {}", iptc_strerror(errno));
        return std::unexpected(RoutingError::IptablesError);
    }

    logger::info("[ROUTING] iptables mangle rules committed");
    return {};
}

void RoutingPolicyManager::removeIptablesMarkRules() noexcept
{
    struct xtc_handle* raw = iptc_init(kMangleTable);
    if (!raw) {
        logger::warn("[ROUTING] cleanup: iptc_init(mangle) failed: {}", iptc_strerror(errno));
        return;
    }
    auto h = std::unique_ptr<xtc_handle, decltype(&iptc_free)>{raw, iptc_free};

    if (!iptc_is_chain(kNetserviceChain, h.get())) {
        return; // nothing to clean up
    }

    // Delete the OUTPUT → NETSERVICE-MARK jump rule (find by target name)
    unsigned int ruleNum = 0;
    for (const ipt_entry* e = iptc_first_rule(kOutputChain, h.get());
         e != nullptr;
         e = iptc_next_rule(e, h.get()), ++ruleNum) {
        if (std::string_view{iptc_get_target(e, h.get())} == kNetserviceChain) {
            if (!iptc_delete_num_entry(kOutputChain, ruleNum, h.get())) {
                logger::warn("[ROUTING] cleanup: delete OUTPUT jump failed: {}",
                             iptc_strerror(errno));
            }
            break;
        }
    }

    // Flush then delete the NETSERVICE-MARK chain
    if (!iptc_flush_entries(kNetserviceChain, h.get())) {
        logger::warn("[ROUTING] cleanup: flush {} failed: {}",
                     kNetserviceChain, iptc_strerror(errno));
    }
    if (!iptc_delete_chain(kNetserviceChain, h.get())) {
        logger::warn("[ROUTING] cleanup: delete chain {} failed: {}",
                     kNetserviceChain, iptc_strerror(errno));
    }

    if (!iptc_commit(h.get())) {
        logger::warn("[ROUTING] cleanup: iptc_commit failed: {}", iptc_strerror(errno));
    } else {
        logger::info("[ROUTING] cleanup: iptables mangle rules removed");
    }
}

void RoutingPolicyManager::cleanup() noexcept
{
    // P2-T2: remove iptables rules first (before cgroup dirs disappear)
    removeIptablesMarkRules();
    removeIptablesMarkRules();

    // P2-T1: remove cgroup directories in reverse order (children before parents).
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
