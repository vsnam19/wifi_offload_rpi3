#!/bin/sh
# setup-test-vlans.sh — Auto-configure test VLAN subinterfaces on eth0
#
# Creates eth0.100 / eth0.200 / eth0.300 with fixed IPs for the
# host-side test bench (VLAN ID matches routing table ID):
#
#   VLAN 100  →  172.16.1.2/24   (multipath / WiFi path)
#   VLAN 200  →  172.16.2.2/24   (lte_b2c path)
#   VLAN 300  →  172.16.3.2/24   (lte_b2b path)
#
# Intended for DEVELOPMENT / TEST only — not for production.
# Deployed via netservice-image.bb as a oneshot systemd service.

set -e

# VLAN support
modprobe 8021q

# iptables kernel modules required by wifi-offload-manager routing setup
modprobe ip_tables
modprobe iptable_filter
modprobe iptable_mangle
modprobe xt_mark
modprobe xt_cgroup

for vid in 100 200 300; do
    case "$vid" in
        100) ip="172.16.1.2" ;;
        200) ip="172.16.2.2" ;;
        300) ip="172.16.3.2" ;;
    esac

    iface="eth0.${vid}"

    # Create VLAN subinterface (idempotent)
    if ! ip link show "${iface}" > /dev/null 2>&1; then
        ip link add link eth0 name "${iface}" type vlan id "${vid}"
    fi

    ip link set "${iface}" up
    # Remove stale addresses before adding (avoids EEXIST on re-run)
    ip addr flush dev "${iface}" 2>/dev/null || true
    ip addr add "${ip}/24" dev "${iface}"

    logger -t setup-test-vlans "configured ${iface} → ${ip}/24"
done
