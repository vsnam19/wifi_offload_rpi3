#!/bin/bash
# TEST ONLY — VLAN subinterfaces + IP assignment + cross-VLAN isolation + internet NAT
#
# IP scheme (VLAN index as 3rd octet, VLAN 300 > 255 so index-based):
#   VLAN 100  →  host 172.16.1.1/24   rpi 172.16.1.2/24  (multipath)
#   VLAN 200  →  host 172.16.2.1/24   rpi 172.16.2.2/24  (lte_b2c)
#   VLAN 300  →  host 172.16.3.1/24   rpi 172.16.3.2/24  (lte_b2b)
#
# Policy:
#   - Cross-VLAN forwarding DENIED  (no 172.16.1.x ↔ 172.16.2.x ↔ 172.16.3.x)
#   - Each VLAN can reach internet via MASQUERADE on INTERNET_IFACE
#   - RPi routing tables 100/200/300 get a default route via host VLAN gateway
#
# Usage:
#   sudo ./scripts/setup-test-vlans.sh           # bring up
#   sudo ./scripts/setup-test-vlans.sh down      # tear down
#   HOST_IFACE=eth1 INTERNET_IFACE=wlan0 sudo ./scripts/setup-test-vlans.sh

HOST_IFACE="${HOST_IFACE:-enp2s0}"
INTERNET_IFACE="${INTERNET_IFACE:-wlp4s0}"
RPI_SSH="${RPI_SSH:-root@172.16.45.2}"

ACTION="${1:-up}"

# VLAN ID → subnet third octet
declare -A OCT=([100]=1 [200]=2 [300]=3)
# VLAN ID → RPi routing table
declare -A TABLE=([100]=100 [200]=200 [300]=300)
# RPi VLAN interface base
RPI_IFACE="eth0"

# ── helpers ────────────────────────────────────────────────────────────────
# ipt_add inserts FORWARD rules at position 1 to stay ahead of Docker's DROP rules.
# Docker uses `-I` to insert its rules at the top; `-A` would land behind them.
ipt_add()  {
    if [[ "$1" == "FORWARD" ]]; then
        iptables -C "$@" 2>/dev/null || iptables -I "$@" 1
    else
        iptables -C "$@" 2>/dev/null || iptables -A "$@"
    fi
}
ipt_del()  { iptables -D "$@" 2>/dev/null || true; }
nat_add()  { iptables -t nat -C "$@" 2>/dev/null || iptables -t nat -A "$@"; }
nat_del()  { iptables -t nat -D "$@" 2>/dev/null || true; }

# ── down ──────────────────────────────────────────────────────────────────
if [[ "$ACTION" == "down" ]]; then
    echo "==> Removing FORWARD ACCEPT rules"
    for vid in 100 200 300; do
        iface="${HOST_IFACE}.${vid}"
        ipt_del FORWARD -i "${iface}" -o "${INTERNET_IFACE}" -j ACCEPT
        ipt_del FORWARD -i "${INTERNET_IFACE}" -o "${iface}" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    done

    echo "==> Removing cross-VLAN DROP rules"
    for v1 in 100 200 300; do
        for v2 in 100 200 300; do
            [[ "$v1" -ge "$v2" ]] && continue
            ipt_del FORWARD -i "${HOST_IFACE}.${v1}" -o "${HOST_IFACE}.${v2}" -j DROP
            ipt_del FORWARD -i "${HOST_IFACE}.${v2}" -o "${HOST_IFACE}.${v1}" -j DROP
        done
    done

    echo "==> Removing NAT MASQUERADE rules"
    for vid in 100 200 300; do
        nat_del POSTROUTING -s "172.16.${OCT[$vid]}.0/24" -o "${INTERNET_IFACE}" -j MASQUERADE
    done

    echo "==> Removing VLAN interfaces"
    for vid in 100 200 300; do
        ip link del "${HOST_IFACE}.${vid}" 2>/dev/null && echo "  removed ${HOST_IFACE}.${vid}" || true
    done

    echo "==> Removing RPi default routes from routing tables"
    ssh "${RPI_SSH}" '
        ip route del default table 100 2>/dev/null && echo "  table 100 cleared" || true
        ip route del default table 200 2>/dev/null && echo "  table 200 cleared" || true
        ip route del default table 300 2>/dev/null && echo "  table 300 cleared" || true
    ' 2>/dev/null || echo "  (ssh to RPi failed — manual cleanup may be needed)"

    echo "==> Removing source-based ip rules on RPi"
    ssh "${RPI_SSH}" '
        ip rule del from 172.16.1.2 lookup 100 prio 100 2>/dev/null || true
        ip rule del from 172.16.2.2 lookup 200 prio 100 2>/dev/null || true
        ip rule del from 172.16.3.2 lookup 300 prio 100 2>/dev/null || true
    ' 2>/dev/null || echo "  (ssh to RPi failed — manual ip rule del needed)"

    sysctl -qw net.ipv4.ip_forward=0
    echo "Done."
    exit 0
fi

# ── up ────────────────────────────────────────────────────────────────────
echo "==> Enabling IP forwarding"
sysctl -qw net.ipv4.ip_forward=1

echo "==> Loading 802.1q"
modprobe 8021q

echo "==> Creating VLAN interfaces + addresses"
for vid in 100 200 300; do
    oct="${OCT[$vid]}"
    iface="${HOST_IFACE}.${vid}"
    ip link add link "${HOST_IFACE}" name "${iface}" type vlan id "${vid}" 2>/dev/null \
        || echo "  ${iface} already exists"
    ip link set "${iface}" up
    ip addr add "172.16.${oct}.1/24" dev "${iface}" 2>/dev/null \
        || echo "  ${iface} addr 172.16.${oct}.1/24 already set"
done

echo "==> Adding FORWARD ACCEPT rules (VLAN → internet, ESTABLISHED return)"
# Needed when Docker or other rules tighten the default FORWARD policy.
for vid in 100 200 300; do
    iface="${HOST_IFACE}.${vid}"
    ipt_add FORWARD -i "${iface}" -o "${INTERNET_IFACE}" -j ACCEPT
    ipt_add FORWARD -i "${INTERNET_IFACE}" -o "${iface}" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    echo "  ACCEPT: ${iface} ↔ ${INTERNET_IFACE} (established)"
done

echo "==> Adding cross-VLAN DROP rules in FORWARD chain"
for v1 in 100 200 300; do
    for v2 in 100 200 300; do
        [[ "$v1" -ge "$v2" ]] && continue
        ipt_add FORWARD -i "${HOST_IFACE}.${v1}" -o "${HOST_IFACE}.${v2}" -j DROP
        ipt_add FORWARD -i "${HOST_IFACE}.${v2}" -o "${HOST_IFACE}.${v1}" -j DROP
        echo "  DROP: ${HOST_IFACE}.${v1} ↔ ${HOST_IFACE}.${v2}"
    done
done

echo "==> Adding MASQUERADE rules (VLAN → ${INTERNET_IFACE})"
for vid in 100 200 300; do
    oct="${OCT[$vid]}"
    nat_add POSTROUTING -s "172.16.${oct}.0/24" -o "${INTERNET_IFACE}" -j MASQUERADE
    echo "  172.16.${oct}.0/24 → MASQUERADE via ${INTERNET_IFACE}"
done

echo "==> Adding default routes on RPi routing tables via VLAN gateways"
# -o StrictHostKeyChecking=no because sudo may not have RPi key in known_hosts
ssh -o StrictHostKeyChecking=no -o BatchMode=yes "${RPI_SSH}" "
    ip route replace default via 172.16.1.1 dev ${RPI_IFACE}.100 table 100 && echo '  table 100: via 172.16.1.1'
    ip route replace default via 172.16.2.1 dev ${RPI_IFACE}.200 table 200 && echo '  table 200: via 172.16.2.1'
    ip route replace default via 172.16.3.1 dev ${RPI_IFACE}.300 table 300 && echo '  table 300: via 172.16.3.1'
" 2>/dev/null || {
    echo "  (ssh failed — adding RPi routes manually via user session)"
    sudo -u "${SUDO_USER:-$USER}" ssh "${RPI_SSH}" "
        ip route replace default via 172.16.1.1 dev ${RPI_IFACE}.100 table 100 && echo '  table 100: via 172.16.1.1'
        ip route replace default via 172.16.2.1 dev ${RPI_IFACE}.200 table 200 && echo '  table 200: via 172.16.2.1'
        ip route replace default via 172.16.3.1 dev ${RPI_IFACE}.300 table 300 && echo '  table 300: via 172.16.3.1'
    "
}

echo "==> Adding source-based ip rules on RPi (ensures correct table per VLAN source IP)"
ssh -o StrictHostKeyChecking=no -o BatchMode=yes "${RPI_SSH}" '
    ip rule add from 172.16.1.2 lookup 100 prio 100 2>/dev/null && echo "  from 172.16.1.2 → table 100" || echo "  from 172.16.1.2 → table 100 (already exists)"
    ip rule add from 172.16.2.2 lookup 200 prio 100 2>/dev/null && echo "  from 172.16.2.2 → table 200" || echo "  from 172.16.2.2 → table 200 (already exists)"
    ip rule add from 172.16.3.2 lookup 300 prio 100 2>/dev/null && echo "  from 172.16.3.2 → table 300" || echo "  from 172.16.3.2 → table 300 (already exists)"
' 2>/dev/null || echo "  (ssh to RPi failed — manual ip rule add needed)"

echo ""
echo "=== Host VLAN interfaces + addresses ==="
ip addr show | grep -A2 "${HOST_IFACE}\." | grep -E "^\s*(inet |[0-9]+:)"
echo ""
echo "=== Cross-VLAN DROP rules ==="
iptables -L FORWARD -n | grep "${HOST_IFACE}"
echo ""
echo "=== NAT MASQUERADE ==="
iptables -t nat -L POSTROUTING -n | grep "172.16"
