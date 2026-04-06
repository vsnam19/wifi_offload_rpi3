#!/bin/bash
# TEST ONLY — create VLAN subinterfaces + assign IPs on host for P2-T5 verification
#
# IP scheme (VLAN index as 3rd octet):
#   eth0.100 / enp2s0.100  →  172.16.1.1/24  (host)  172.16.1.2/24  (rpi)
#   eth0.200 / enp2s0.200  →  172.16.2.1/24  (host)  172.16.2.2/24  (rpi)
#   eth0.300 / enp2s0.300  →  172.16.3.1/24  (host)  172.16.3.2/24  (rpi)
#   (VLAN 300 > 255 so third octet is index, not VLAN ID)
#
# Run once with: sudo ./scripts/setup-test-vlans.sh
# Remove with:   sudo ./scripts/setup-test-vlans.sh down
set -e

HOST_IFACE="${HOST_IFACE:-enp2s0}"
ACTION="${1:-up}"

# VLAN ID → third octet mapping
declare -A VLAN_SUBNET=([100]=1 [200]=2 [300]=3)

if [[ "$ACTION" == "down" ]]; then
    for vid in "${!VLAN_SUBNET[@]}"; do
        ip link del "${HOST_IFACE}.${vid}" 2>/dev/null && echo "Removed ${HOST_IFACE}.${vid}" || true
    done
    exit 0
fi

modprobe 8021q
for vid in 100 200 300; do
    oct="${VLAN_SUBNET[$vid]}"
    iface="${HOST_IFACE}.${vid}"
    ip link add link "${HOST_IFACE}" name "${iface}" type vlan id "${vid}" 2>/dev/null \
        || echo "${iface} already exists"
    ip link set "${iface}" up
    ip addr add "172.16.${oct}.1/24" dev "${iface}" 2>/dev/null \
        || echo "${iface} addr 172.16.${oct}.1/24 already set"
done

echo "=== Host VLAN interfaces + addresses ==="
ip addr show | grep -A3 "${HOST_IFACE}\."
