// mptcp_manager.cpp — MPTCP PM endpoint management via Generic Netlink
//
// Kernel API: family "mptcp_pm", commands MPTCP_PM_CMD_ADD_ADDR / DEL_ADDR
// Attributes: nested MPTCP_PM_ATTR_ADDR containing:
//   MPTCP_PM_ADDR_ATTR_FAMILY  u16  AF_INET
//   MPTCP_PM_ADDR_ATTR_ADDR4   struct in_addr  (IPv4 address of the interface)
//   MPTCP_PM_ADDR_ATTR_FLAGS   u32  MPTCP_PM_ADDR_FLAG_SUBFLOW
//   MPTCP_PM_ADDR_ATTR_IF_IDX  s32  if_nametoindex(iface)
//
// See: include/uapi/linux/mptcp.h

#include "mptcp/mptcp_manager.hpp"
#include "common/logger.hpp"

#include <libmnl/libmnl.h>
#include <linux/genetlink.h>
#include <linux/mptcp.h>

#include <net/if.h>         // if_nametoindex
#include <ifaddrs.h>        // getifaddrs
#include <arpa/inet.h>      // inet_pton

#include <cstring>
#include <cerrno>
#include <ctime>
#include <format>
#include <memory>

namespace netservice {

// ── constructor ───────────────────────────────────────────────────────────

MptcpManager::MptcpManager(std::vector<PathClassConfig> classes)
    : classes_{std::move(classes)}
{}

// ── helpers ───────────────────────────────────────────────────────────────

// Get the first IPv4 address assigned to the named interface.
// Returns 0 on failure (address not yet assigned).
static uint32_t ifaceAddr4(const char* ifname) noexcept
{
    struct ifaddrs* ifa_list{nullptr};
    if (::getifaddrs(&ifa_list) != 0) return 0u;

    uint32_t result = 0u;
    for (auto* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (std::strcmp(ifa->ifa_name, ifname) != 0) continue;
        result = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr;
        break;
    }
    ::freeifaddrs(ifa_list);
    return result;
}

// ── Generic Netlink family resolution ────────────────────────────────────

// Callback used by mnl_cb_run to parse CTRL_CMD_GETFAMILY reply.
static int genlFamilyParseId(const nlmsghdr* nlh, void* data) noexcept
{
    auto* id = static_cast<int*>(data);
    const auto* genl = static_cast<const genlmsghdr*>(mnl_nlmsg_get_payload(nlh));
    // Manually iterate attributes after genlmsghdr — avoids void* implicit cast
    // that mnl_attr_for_each relies on (valid in C, invalid in C++)
    const char* attrStart = reinterpret_cast<const char*>(genl) + sizeof(*genl);
    const char* msgEnd    = reinterpret_cast<const char*>(nlh) + nlh->nlmsg_len;
    for (auto* a = reinterpret_cast<struct nlattr*>(const_cast<char*>(attrStart));
         mnl_attr_ok(a, static_cast<int>(msgEnd - reinterpret_cast<const char*>(a)));
         a = mnl_attr_next(a)) {
        if (mnl_attr_get_type(a) == CTRL_ATTR_FAMILY_ID) {
            *id = static_cast<int>(mnl_attr_get_u16(a));
            break;
        }
    }
    return MNL_CB_OK;
}

std::expected<int, RoutingError>
MptcpManager::resolveGenlFamily() noexcept
{
    char buf[4096];

    mnl_socket* raw = mnl_socket_open(NETLINK_GENERIC);
    if (!raw) {
        logger::error("[MPTCP] mnl_socket_open failed: errno={} ({})", errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }
    auto nl = std::unique_ptr<mnl_socket, decltype(&mnl_socket_close)>{raw, mnl_socket_close};

    if (mnl_socket_bind(nl.get(), 0, MNL_SOCKET_AUTOPID) < 0) {
        logger::error("[MPTCP] mnl_socket_bind failed: errno={} ({})", errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    unsigned int seq = static_cast<unsigned int>(time(nullptr));
    unsigned int portid = mnl_socket_get_portid(nl.get());

    // Build CTRL_CMD_GETFAMILY request
    nlmsghdr* nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type  = GENL_ID_CTRL;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq   = seq;

    auto* genl = static_cast<genlmsghdr*>(mnl_nlmsg_put_extra_header(nlh, sizeof(genlmsghdr)));
    genl->cmd = CTRL_CMD_GETFAMILY;

    mnl_attr_put_strz(nlh, CTRL_ATTR_FAMILY_NAME, MPTCP_PM_NAME);

    if (mnl_socket_sendto(nl.get(), nlh, nlh->nlmsg_len) < 0) {
        logger::error("[MPTCP] sendto CTRL_CMD_GETFAMILY failed: errno={}", errno);
        return std::unexpected(RoutingError::NetlinkError);
    }

    int familyId = -1;
    int ret = mnl_socket_recvfrom(nl.get(), buf, sizeof(buf));
    while (ret > 0) {
        ret = mnl_cb_run(buf, static_cast<size_t>(ret), seq, portid, genlFamilyParseId, &familyId);
        if (ret <= MNL_CB_STOP) break;
        ret = mnl_socket_recvfrom(nl.get(), buf, sizeof(buf));
    }

    if (ret < 0 || familyId < 0) {
        logger::error("[MPTCP] failed to resolve mptcp_pm genl family: errno={}", errno);
        return std::unexpected(RoutingError::NetlinkError);
    }

    return familyId;
}

// ── Send one ADD_ADDR / DEL_ADDR command ────────────────────────────────

std::expected<void, RoutingError>
MptcpManager::sendEndpointCmd(int genlFamilyId, uint8_t cmd,
                              const char* iface, uint32_t addr4) noexcept
{
    char buf[4096];

    mnl_socket* raw = mnl_socket_open(NETLINK_GENERIC);
    if (!raw) {
        logger::error("[MPTCP] mnl_socket_open failed: errno={} ({})", errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }
    auto nl = std::unique_ptr<mnl_socket, decltype(&mnl_socket_close)>{raw, mnl_socket_close};

    if (mnl_socket_bind(nl.get(), 0, MNL_SOCKET_AUTOPID) < 0) {
        return std::unexpected(RoutingError::NetlinkError);
    }

    unsigned int seq    = static_cast<unsigned int>(time(nullptr));
    unsigned int portid = mnl_socket_get_portid(nl.get());

    nlmsghdr* nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type  = static_cast<uint16_t>(genlFamilyId);
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq   = seq;

    auto* genl = static_cast<genlmsghdr*>(mnl_nlmsg_put_extra_header(nlh, sizeof(genlmsghdr)));
    genl->cmd     = cmd;
    genl->reserved = 0;

    // Nested MPTCP_PM_ATTR_ADDR
    struct nlattr* nested = mnl_attr_nest_start(nlh, MPTCP_PM_ATTR_ADDR);

    uint16_t family = AF_INET;
    mnl_attr_put(nlh, MPTCP_PM_ADDR_ATTR_FAMILY, sizeof(family), &family);
    mnl_attr_put(nlh, MPTCP_PM_ADDR_ATTR_ADDR4,  sizeof(addr4),  &addr4);

    if (cmd == MPTCP_PM_CMD_ADD_ADDR) {
        uint32_t flags = MPTCP_PM_ADDR_FLAG_SUBFLOW;
        mnl_attr_put_u32(nlh, MPTCP_PM_ADDR_ATTR_FLAGS, flags);

        int32_t ifindex = static_cast<int32_t>(if_nametoindex(iface));
        if (ifindex == 0) {
            logger::warn("[MPTCP] interface not found: {}", iface);
            return std::unexpected(RoutingError::NetlinkError);
        }
        mnl_attr_put_u32(nlh, MPTCP_PM_ADDR_ATTR_IF_IDX, static_cast<uint32_t>(ifindex));
    }

    mnl_attr_nest_end(nlh, nested);

    if (mnl_socket_sendto(nl.get(), nlh, nlh->nlmsg_len) < 0) {
        logger::error("[MPTCP] sendto cmd={} failed: errno={} ({})", cmd, errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    char ackbuf[4096];
    int ret = mnl_socket_recvfrom(nl.get(), ackbuf, sizeof(ackbuf));
    while (ret > 0) {
        ret = mnl_cb_run(ackbuf, static_cast<size_t>(ret), seq, portid, nullptr, nullptr);
        if (ret <= MNL_CB_STOP) break;
        ret = mnl_socket_recvfrom(nl.get(), ackbuf, sizeof(ackbuf));
    }

    if (ret < 0) {
        // EEXIST on ADD and ENOENT on DEL are expected in idempotent calls
        if ((cmd == MPTCP_PM_CMD_ADD_ADDR && errno == EEXIST) ||
            (cmd == MPTCP_PM_CMD_DEL_ADDR && errno == ENOENT)) {
            return {};
        }
        logger::error("[MPTCP] cmd={} iface={} errno={} ({})", cmd, iface, errno, std::strerror(errno));
        return std::unexpected(RoutingError::NetlinkError);
    }

    return {};
}

// ── Public API ────────────────────────────────────────────────────────────

std::expected<void, RoutingError>
MptcpManager::addEndpoints()
{
    auto familyId = resolveGenlFamily();
    if (!familyId) return std::unexpected(familyId.error());

    for (const auto& cls : classes_) {
        if (!cls.mptcpEnabled) continue;

        for (const auto& iface : cls.interfaces) {
            uint32_t addr4 = ifaceAddr4(iface.c_str());
            if (addr4 == 0) {
                logger::warn("[MPTCP] no IPv4 address on {}, skipping endpoint", iface);
                continue;
            }

            char addrStr[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &addr4, addrStr, sizeof(addrStr));

            auto result = sendEndpointCmd(*familyId, MPTCP_PM_CMD_ADD_ADDR,
                                          iface.c_str(), addr4);
            if (!result) {
                logger::error("[MPTCP] addEndpoints failed: class={} iface={} addr={}",
                              cls.id, iface, addrStr);
                return result;
            }
            logger::info("[MPTCP] endpoint added: class={} iface={} addr={} (subflow)",
                         cls.id, iface, addrStr);
        }
    }
    return {};
}

void MptcpManager::removeEndpoints() noexcept
{
    // Flush all PM endpoints via MPTCP_PM_CMD_FLUSH_ADDRS — simpler than
    // tracking individual IDs, and correct for daemon shutdown.
    auto familyId = resolveGenlFamily();
    if (!familyId) {
        logger::warn("[MPTCP] removeEndpoints: failed to resolve genl family");
        return;
    }

    char buf[4096];
    mnl_socket* raw = mnl_socket_open(NETLINK_GENERIC);
    if (!raw) return;
    auto nl = std::unique_ptr<mnl_socket, decltype(&mnl_socket_close)>{raw, mnl_socket_close};

    if (mnl_socket_bind(nl.get(), 0, MNL_SOCKET_AUTOPID) < 0) return;

    unsigned int seq    = static_cast<unsigned int>(time(nullptr));
    unsigned int portid = mnl_socket_get_portid(nl.get());

    nlmsghdr* nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type  = static_cast<uint16_t>(*familyId);
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq   = seq;

    auto* genl = static_cast<genlmsghdr*>(mnl_nlmsg_put_extra_header(nlh, sizeof(genlmsghdr)));
    genl->cmd     = MPTCP_PM_CMD_FLUSH_ADDRS;
    genl->reserved = 0;

    // FLUSH_ADDRS requires a dummy nested MPTCP_PM_ATTR_ADDR (kernel enforces it)
    struct nlattr* nested = mnl_attr_nest_start(nlh, MPTCP_PM_ATTR_ADDR);
    uint16_t family = AF_INET;
    mnl_attr_put(nlh, MPTCP_PM_ADDR_ATTR_FAMILY, sizeof(family), &family);
    mnl_attr_nest_end(nlh, nested);

    if (mnl_socket_sendto(nl.get(), nlh, nlh->nlmsg_len) < 0) {
        logger::error("[MPTCP] FLUSH_ADDRS sendto failed: errno={}", errno);
        return;
    }

    char ackbuf[4096];
    int ret = mnl_socket_recvfrom(nl.get(), ackbuf, sizeof(ackbuf));
    while (ret > 0) {
        ret = mnl_cb_run(ackbuf, static_cast<size_t>(ret), seq, portid, nullptr, nullptr);
        if (ret <= MNL_CB_STOP) break;
        ret = mnl_socket_recvfrom(nl.get(), ackbuf, sizeof(ackbuf));
    }

    if (ret < 0 && errno != ENOENT) {
        logger::error("[MPTCP] FLUSH_ADDRS failed: errno={} ({})", errno, std::strerror(errno));
        return;
    }

    logger::info("[MPTCP] all endpoints flushed");
}

} // namespace netservice
