// routing_policy_manager.cpp — P2-T1/P2-T2/P2-T3: cgroup + iptables MARK + ip rules
//                               P4-T3/P4-T4: addDefaultRoute / removeDefaultRoute

#include "routing/routing_policy_manager.hpp"
#include "common/logger.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
#include <vector>

// For if_nametoindex
#include <net/if.h>

// libiptc and kernel netfilter headers for P2-T2
#include <libiptc/libiptc.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_cgroup.h>
#include <linux/netfilter/xt_mark.h>

// Netlink / libmnl headers for P2-T3
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <linux/fib_rules.h>

namespace netservice {

namespace {

// Root of the cgroup net_cls hierarchy.
// With cgroup v1 (systemd.unified_cgroup_hierarchy=0), this is always
// /sys/fs/cgroup/net_cls/.  The path is read from PathClassConfig::cgroupPath
// per class, so this constant is only used for log messages.
constexpr std::string_view kCgroupNetClsRoot = "/sys/fs/cgroup/net_cls";

// ── P2-T2 constants ───────────────────────────────────────────────
constexpr const char* kMangleTable     = "mangle";
constexpr const char* kFilterTable     = "filter";
constexpr const char* kOutputChain     = "OUTPUT";
constexpr const char* kNetserviceChain = "NETSERVICE-MARK";
constexpr const char* kIsoChain        = "NETSERVICE-ISO";

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

// Build a heap buffer for:
//   -o <outiface> -m mark --mark <mark>/0xffffffff -j DROP
// Used for strict_isolation safety-net rules in the filter table.
//
// Target: standard target with empty name + DROP verdict (-NF_DROP - 1 = -1).
// Match:  "mark" revision 1 (xt_mark_mtinfo1: mark + mask + invert).
std::vector<uint8_t> buildDropEntry(std::string_view outiface, uint32_t mark)
{
    constexpr size_t kMatchSz  = xtAlign(sizeof(xt_entry_match) + sizeof(xt_mark_mtinfo1));
    constexpr size_t kTargetSz = xtAlign(sizeof(xt_standard_target));
    constexpr size_t kHdrSz    = xtAlign(sizeof(ipt_entry));
    constexpr size_t kTotal    = kHdrSz + kMatchSz + kTargetSz;

    std::vector<uint8_t> buf(kTotal, 0);

    auto* e  = reinterpret_cast<ipt_entry*>          (buf.data());
    auto* m  = reinterpret_cast<xt_entry_match*>     (buf.data() + kHdrSz);
    auto* mk = reinterpret_cast<xt_mark_mtinfo1*>    (m->data);
    auto* t  = reinterpret_cast<xt_standard_target*> (buf.data() + kHdrSz + kMatchSz);

    // Output interface filter (in ipt_ip, no extension needed)
    const std::size_t ifLen = std::min(outiface.size(), static_cast<std::size_t>(IFNAMSIZ - 1));
    std::memcpy(e->ip.outiface,      outiface.data(), ifLen);
    std::memset(e->ip.outiface_mask, 0xFF, ifLen + 1); // +1 covers the '\0'

    // Match: "mark" revision 1
    m->u.user.match_size = static_cast<uint16_t>(kMatchSz);
    std::strncpy(m->u.user.name, "mark", XT_EXTENSION_MAXNAMELEN - 1);
    m->u.user.revision  = 1;
    mk->mark            = mark;
    mk->mask            = 0xFFFFFFFFu;
    mk->invert          = 0;

    // Target: standard DROP
    // iptcc_map_target() dispatches on user.name:
    //   ""     → IPTCC_R_FALLTHROUGH (no-op!)
    //   "DROP" → iptcc_standard_map(r, -NF_DROP-1) → IPTCC_R_STANDARD ✓
    // iptcc_standard_map overwrites user.name → "" and verdict → -NF_DROP-1
    // before the table is sent to the kernel via setsockopt.
    t->target.u.user.target_size = static_cast<uint16_t>(kTargetSz);
    std::strncpy(t->target.u.user.name, "DROP", XT_EXTENSION_MAXNAMELEN - 1);
    t->verdict = -NF_DROP - 1; // = -1  (NF_DROP = 0)

    e->target_offset = static_cast<uint16_t>(kHdrSz + kMatchSz);
    e->next_offset   = static_cast<uint16_t>(kTotal);

    return buf;
}

// ── P2-T3 constants ────────────────────────────────────────────────────────
// Priority slightly below the default rules (32766 = default, 32767 = local).
constexpr uint32_t kIpRulePriority = 32765u;

// Single helper that builds and sends one RTM_NEWRULE or RTM_DELRULE message.
// For RTM_NEWRULE, pass NLM_F_CREATE | NLM_F_EXCL as extraFlags.
// For RTM_DELRULE, pass 0 as extraFlags.
//
// EEXIST (add of existing rule) and ENOENT (delete of absent rule) are
// silently treated as success because the invariant is already satisfied.
[[nodiscard]] static std::expected<void, RoutingError>
sendNetlinkFwmarkRule(uint16_t msgType, uint16_t extraFlags,
                      uint32_t fwmark, uint32_t table)
{
    // 4096 bytes is more than enough for a single rule message.
    char buf[4096];

    struct mnl_socket* raw = mnl_socket_open(NETLINK_ROUTE);
    if (!raw) {
        logger::error("[ROUTING] mnl_socket_open failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }
    auto nl = std::unique_ptr<mnl_socket, decltype(&mnl_socket_close)>{
        raw, mnl_socket_close};

    if (mnl_socket_bind(nl.get(), 0, MNL_SOCKET_AUTOPID) < 0) {
        logger::error("[ROUTING] mnl_socket_bind failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    const uint32_t seq    = static_cast<uint32_t>(std::time(nullptr));
    const uint32_t portid = mnl_socket_get_portid(nl.get());

    // Build the Netlink message.
    struct nlmsghdr* nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type  = msgType;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK
                       | static_cast<uint16_t>(extraFlags);
    nlh->nlmsg_seq   = seq;

    // rtmsg header — describes the rule family and basic properties.
    auto* rtm = static_cast<struct rtmsg*>(
        mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg)));
    rtm->rtm_family   = AF_INET;
    rtm->rtm_dst_len  = 0;
    rtm->rtm_src_len  = 0;
    rtm->rtm_tos      = 0;
    rtm->rtm_table    = RT_TABLE_UNSPEC; // actual table via FRA_TABLE attribute
    rtm->rtm_protocol = RTPROT_BOOT;
    rtm->rtm_scope    = RT_SCOPE_UNIVERSE;
    rtm->rtm_type     = RTN_UNICAST;
    rtm->rtm_flags    = 0;

    mnl_attr_put_u32(nlh, FRA_FWMARK,   fwmark);
    mnl_attr_put_u32(nlh, FRA_TABLE,    table);
    mnl_attr_put_u32(nlh, FRA_PRIORITY, kIpRulePriority);

    if (mnl_socket_sendto(nl.get(), nlh, nlh->nlmsg_len) < 0) {
        logger::error("[ROUTING] mnl_socket_sendto failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    const ssize_t nrecv = mnl_socket_recvfrom(nl.get(), buf, sizeof(buf));
    if (nrecv < 0) {
        if ((msgType == RTM_NEWRULE && errno == EEXIST) ||
            (msgType == RTM_DELRULE && errno == ENOENT)) {
            return {};
        }
        logger::error("[ROUTING] mnl_socket_recvfrom failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    const int ret = mnl_cb_run(buf, static_cast<size_t>(nrecv), seq, portid,
                               nullptr, nullptr);
    if (ret < 0) {
        if ((msgType == RTM_NEWRULE && errno == EEXIST) ||
            (msgType == RTM_DELRULE && errno == ENOENT)) {
            return {};
        }
        logger::error("[ROUTING] netlink rule op=0x{:04x} failed: errno={} ({})",
                      msgType, errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    return {};
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

// ── P2-T3 ─────────────────────────────────────────────────────────

std::expected<void, RoutingError> RoutingPolicyManager::addIpRules()
{
    logger::info("[ROUTING] adding ip fwmark rules ({} class(es))", classes_.size());

    for (const auto& cls : classes_) {
        auto result = sendNetlinkFwmarkRule(
            RTM_NEWRULE,
            NLM_F_CREATE | NLM_F_EXCL,
            cls.mark,
            static_cast<uint32_t>(cls.routingTable));
        if (!result) {
            return result;
        }
        logger::info("[ROUTING] ip rule added: id={} fwmark=0x{:x} lookup={}",
                     cls.id, cls.mark, cls.routingTable);
    }

    logger::info("[ROUTING] ip fwmark rules ready");
    return {};
}

void RoutingPolicyManager::removeIpRules() noexcept
{
    for (const auto& cls : classes_) {
        auto result = sendNetlinkFwmarkRule(
            RTM_DELRULE, 0,
            cls.mark,
            static_cast<uint32_t>(cls.routingTable));
        if (!result) {
            logger::warn("[ROUTING] cleanup: failed to remove ip rule: id={} fwmark=0x{:x}",
                         cls.id, cls.mark);
        } else {
            logger::info("[ROUTING] cleanup: ip rule removed: id={} fwmark=0x{:x}",
                         cls.id, cls.mark);
        }
    }
}

// ── P2-T4 ─────────────────────────────────────────────────────────

std::expected<void, RoutingError> RoutingPolicyManager::addDropRules()
{
    // Check whether any class requires strict isolation — skip entirely if not.
    const bool anyIsolation = std::ranges::any_of(classes_,
        [](const auto& c) { return c.strictIsolation; });
    if (!anyIsolation) {
        logger::info("[ROUTING] no strict_isolation classes — skipping DROP rules");
        return {};
    }

    logger::info("[ROUTING] setting up strict_isolation DROP rules");

    // ── 1. Open filter table ──────────────────────────────────────
    struct xtc_handle* raw = iptc_init(kFilterTable);
    if (!raw) {
        logger::error("[ROUTING] iptc_init(filter) failed: {}", iptc_strerror(errno));
        return std::unexpected(RoutingError::IptablesError);
    }
    auto h = std::unique_ptr<xtc_handle, decltype(&iptc_free)>{raw, iptc_free};

    // ── 2. Create (or flush) NETSERVICE-ISO user chain ────────────
    if (iptc_is_chain(kIsoChain, h.get())) {
        if (!iptc_flush_entries(kIsoChain, h.get())) {
            logger::error("[ROUTING] flush chain {} failed: {}",
                          kIsoChain, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
        logger::info("[ROUTING] flushed existing chain {}", kIsoChain);
    } else {
        if (!iptc_create_chain(kIsoChain, h.get())) {
            logger::error("[ROUTING] create chain {} failed: {}",
                          kIsoChain, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
    }

    // ── 3. Ensure OUTPUT → NETSERVICE-ISO jump exists ─────────────
    bool jumpExists = false;
    for (const ipt_entry* e = iptc_first_rule(kOutputChain, h.get());
         e != nullptr;
         e = iptc_next_rule(e, h.get())) {
        if (std::string_view{iptc_get_target(e, h.get())} == kIsoChain) {
            jumpExists = true;
            break;
        }
    }
    if (!jumpExists) {
        auto jumpBuf = buildJumpEntry(kIsoChain);
        const auto* je = reinterpret_cast<const ipt_entry*>(jumpBuf.data());
        if (!iptc_append_entry(kOutputChain, je, h.get())) {
            logger::error("[ROUTING] append OUTPUT→{} jump failed: {}",
                          kIsoChain, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
        logger::info("[ROUTING] added OUTPUT jump to {}", kIsoChain);
    } else {
        logger::info("[ROUTING] OUTPUT jump to {} already present", kIsoChain);
    }

    // ── 4. Compute and append DROP rules ──────────────────────────
    // Collect all interfaces mentioned across all classes (deduplicated).
    std::vector<std::string> allIfaces;
    for (const auto& cls : classes_) {
        for (const auto& iface : cls.interfaces) {
            if (std::ranges::find(allIfaces, iface) == allIfaces.end()) {
                allIfaces.push_back(iface);
            }
        }
    }

    auto appendDrop = [&](std::string_view iface, uint32_t mark,
                          std::string_view reason) -> std::expected<void, RoutingError> {
        auto ruleBuf = buildDropEntry(iface, mark);
        const auto* re = reinterpret_cast<const ipt_entry*>(ruleBuf.data());
        if (!iptc_append_entry(kIsoChain, re, h.get())) {
            logger::error("[ROUTING] append DROP rule failed: -o {} --mark 0x{:x}: {}",
                          iface, mark, iptc_strerror(errno));
            return std::unexpected(RoutingError::IptablesError);
        }
        logger::info("[ROUTING] DROP rule added: -o {} --mark 0x{:x}  # {}",
                     iface, mark, reason);
        return {};
    };

    for (const auto& S : classes_) {
        if (!S.strictIsolation) { continue; }

        // (a) S's traffic must not exit on interfaces NOT in S.interfaces
        for (const auto& iface : allIfaces) {
            if (std::ranges::find(S.interfaces, iface) == S.interfaces.end()) {
                auto desc = std::format("{} → no {}", S.id, iface);
                if (auto r = appendDrop(iface, S.mark, desc); !r) { return r; }
            }
        }

        // (b) Every other class B's traffic must not exit on S's interfaces
        for (const auto& B : classes_) {
            if (B.id == S.id) { continue; }
            for (const auto& iface : S.interfaces) {
                auto desc = std::format("{} → no {} (strict_isolation)", B.id, iface);
                if (auto r = appendDrop(iface, B.mark, desc); !r) { return r; }
            }
        }
    }

    // ── 5. Commit ─────────────────────────────────────────────────
    if (!iptc_commit(h.get())) {
        logger::error("[ROUTING] iptc_commit(filter) failed: {}", iptc_strerror(errno));
        return std::unexpected(RoutingError::IptablesError);
    }

    logger::info("[ROUTING] strict_isolation DROP rules committed");
    return {};
}

void RoutingPolicyManager::removeDropRules() noexcept
{
    struct xtc_handle* raw = iptc_init(kFilterTable);
    if (!raw) {
        logger::warn("[ROUTING] cleanup: iptc_init(filter) failed: {}", iptc_strerror(errno));
        return;
    }
    auto h = std::unique_ptr<xtc_handle, decltype(&iptc_free)>{raw, iptc_free};

    if (!iptc_is_chain(kIsoChain, h.get())) {
        return; // nothing to clean up
    }

    // Delete the OUTPUT → NETSERVICE-ISO jump rule
    unsigned int ruleNum = 0;
    for (const ipt_entry* e = iptc_first_rule(kOutputChain, h.get());
         e != nullptr;
         e = iptc_next_rule(e, h.get()), ++ruleNum) {
        if (std::string_view{iptc_get_target(e, h.get())} == kIsoChain) {
            if (!iptc_delete_num_entry(kOutputChain, ruleNum, h.get())) {
                logger::warn("[ROUTING] cleanup: delete OUTPUT→{} jump failed: {}",
                             kIsoChain, iptc_strerror(errno));
            }
            break;
        }
    }

    if (!iptc_flush_entries(kIsoChain, h.get())) {
        logger::warn("[ROUTING] cleanup: flush {} failed: {}",
                     kIsoChain, iptc_strerror(errno));
    }
    if (!iptc_delete_chain(kIsoChain, h.get())) {
        logger::warn("[ROUTING] cleanup: delete chain {} failed: {}",
                     kIsoChain, iptc_strerror(errno));
    }

    if (!iptc_commit(h.get())) {
        logger::warn("[ROUTING] cleanup: iptc_commit(filter) failed: {}", iptc_strerror(errno));
    } else {
        logger::info("[ROUTING] cleanup: DROP rules removed");
    }
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
    // P2-T3: remove ip rules first
    removeIpRules();

    // P2-T4: remove DROP rules
    removeDropRules();

    // P2-T2: remove iptables MARK rules
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

// ─────────────────────────────────────────────────────────────────────────────
// P4-T3 / P4-T4 — dynamic default route management
// ─────────────────────────────────────────────────────────────────────────────

namespace netservice {

namespace {

// Send a single RTM_NEWROUTE / RTM_DELROUTE for a default (0.0.0.0/0) route.
// gwAddr   — gateway in network byte order (0 = omit for RTM_DELROUTE)
// ifindex  — interface index from if_nametoindex()
// table    — routing table number
// msgType  — RTM_NEWROUTE or RTM_DELROUTE
// extraFlags — NLM_F_CREATE | NLM_F_REPLACE for new route; 0 for delete
[[nodiscard]] static std::expected<void, RoutingError>
sendRouteMsg(uint32_t gwAddr, uint32_t ifindex, uint32_t table,
             uint16_t msgType, int extraFlags) noexcept
{
    // MNL_SOCKET_BUFFER_SIZE is not a compile-time constant on all targets;
    // use the standard mnl recommended size of 8 KiB directly.
    char buf[8192];

    struct mnl_socket* raw = mnl_socket_open(NETLINK_ROUTE);
    if (!raw) {
        logger::error("[ROUTING] mnl_socket_open(route) failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }
    auto nl = std::unique_ptr<mnl_socket, decltype(&mnl_socket_close)>{
        raw, mnl_socket_close};

    if (mnl_socket_bind(nl.get(), 0, MNL_SOCKET_AUTOPID) < 0) {
        logger::error("[ROUTING] mnl_socket_bind failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    const uint32_t seq    = static_cast<uint32_t>(std::time(nullptr));
    const uint32_t portid = mnl_socket_get_portid(nl.get());

    struct nlmsghdr* nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type  = msgType;
    nlh->nlmsg_flags = static_cast<uint16_t>(NLM_F_REQUEST | NLM_F_ACK | extraFlags);
    nlh->nlmsg_seq   = seq;

    auto* rtm = static_cast<struct rtmsg*>(
        mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg)));
    rtm->rtm_family   = AF_INET;
    rtm->rtm_dst_len  = 0;        // 0.0.0.0/0 — default route
    rtm->rtm_src_len  = 0;
    rtm->rtm_tos      = 0;
    rtm->rtm_table    = RT_TABLE_UNSPEC;  // real table via RTA_TABLE
    rtm->rtm_protocol = RTPROT_STATIC;
    rtm->rtm_scope    = RT_SCOPE_UNIVERSE;
    rtm->rtm_type     = RTN_UNICAST;
    rtm->rtm_flags    = 0;

    if (gwAddr != 0) {
        mnl_attr_put_u32(nlh, RTA_GATEWAY, gwAddr);
    }
    mnl_attr_put_u32(nlh, RTA_OIF,   ifindex);
    mnl_attr_put_u32(nlh, RTA_TABLE, table);

    if (mnl_socket_sendto(nl.get(), nlh, nlh->nlmsg_len) < 0) {
        logger::error("[ROUTING] sendto(route) failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    const ssize_t nrecv = mnl_socket_recvfrom(nl.get(), buf, sizeof(buf));
    if (nrecv < 0) {
        if ((msgType == RTM_NEWROUTE && errno == EEXIST) ||
            (msgType == RTM_DELROUTE && errno == ENOENT)) {
            return {};
        }
        logger::error("[ROUTING] recvfrom(route) failed: errno={} ({})",
                      errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    const int ret = mnl_cb_run(buf, static_cast<size_t>(nrecv),
                               seq, portid, nullptr, nullptr);
    if (ret < 0) {
        if ((msgType == RTM_NEWROUTE && errno == EEXIST) ||
            (msgType == RTM_DELROUTE && errno == ENOENT)) {
            return {};
        }
        logger::error("[ROUTING] netlink route op=0x{:04x} failed: errno={} ({})",
                      msgType, errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }
    return {};
}

} // anonymous namespace

// ── P4-T3 ──────────────────────────────────────────────────────────────────
std::expected<void, RoutingError>
RoutingPolicyManager::addDefaultRoute(std::string_view iface,
                                      uint32_t gwAddr,
                                      uint32_t routingTable) noexcept
{
    const unsigned int ifindex = if_nametoindex(std::string{iface}.c_str());
    if (ifindex == 0) {
        logger::error("[ROUTING] addDefaultRoute: unknown iface '{}': errno={} ({})",
                      iface, errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    logger::info("[ROUTING] addDefaultRoute: iface={} gw=0x{:08x} table={}",
                 iface, gwAddr, routingTable);

    return sendRouteMsg(gwAddr, ifindex, routingTable, RTM_NEWROUTE,
                        NLM_F_CREATE | NLM_F_REPLACE);
}

// ── P4-T4 ──────────────────────────────────────────────────────────────────
std::expected<void, RoutingError>
RoutingPolicyManager::removeDefaultRoute(std::string_view iface,
                                         uint32_t routingTable) noexcept
{
    const unsigned int ifindex = if_nametoindex(std::string{iface}.c_str());
    if (ifindex == 0) {
        // Interface may already be gone; treat as success.
        logger::warn("[ROUTING] removeDefaultRoute: unknown iface '{}', skipping",
                     iface);
        return {};
    }

    logger::info("[ROUTING] removeDefaultRoute: iface={} table={}", iface, routingTable);

    return sendRouteMsg(0, ifindex, routingTable, RTM_DELROUTE, 0);
}

// ── Gateway query ──────────────────────────────────────────────────────────
uint32_t
RoutingPolicyManager::queryGatewayForIface(std::string_view iface) noexcept
{
    // /proc/net/route columns:
    // Iface  Destination  Gateway  Flags  RefCnt  Use  Metric  Mask  MTU  Window  IRTT
    // All numeric fields are hex, stored little-endian (host byte order on ARM/x86).
    // The default route has Destination == "00000000".
    std::ifstream f{"/proc/net/route"};
    if (!f.is_open()) {
        logger::error("[ROUTING] queryGatewayForIface: cannot open /proc/net/route");
        return 0;
    }

    std::string line;
    std::getline(f, line); // skip header

    while (std::getline(f, line)) {
        std::istringstream ss{line};
        std::string dev, dst, gw;
        if (!(ss >> dev >> dst >> gw)) continue;
        if (dev != iface) continue;
        if (dst != "00000000") continue;  // not the default route

        // gw is in hex, host byte order (LE on ARM/x86)
        // htonl() converts to network byte order for Netlink RTA_GATEWAY.
        unsigned long gwHex{};
        try { gwHex = std::stoul(gw, nullptr, 16); }
        catch (...) { continue; }

        return htonl(static_cast<uint32_t>(gwHex));
    }

    logger::warn("[ROUTING] queryGatewayForIface: no default route for iface={}", iface);
    return 0;
}

} // namespace netservice
