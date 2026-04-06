#!/bin/bash
# TEST ONLY — create VLAN subinterfaces on host for P2-T5 verification
# Run once with: sudo ./scripts/setup-test-vlans.sh
# Remove with:   sudo ./scripts/setup-test-vlans.sh down
set -e

HOST_IFACE="${HOST_IFACE:-enp2s0}"
ACTION="${1:-up}"

vlans=(100 200 300)

if [[ "$ACTION" == "down" ]]; then
    for vid in "${vlans[@]}"; do
        ip link del "${HOST_IFACE}.${vid}" 2>/dev/null && echo "Removed ${HOST_IFACE}.${vid}" || true
    done
    exit 0
fi

modprobe 8021q
for vid in "${vlans[@]}"; do
    ip link add link "${HOST_IFACE}" name "${HOST_IFACE}.${vid}" type vlan id "${vid}" 2>/dev/null \
        || echo "${HOST_IFACE}.${vid} already exists"
    ip link set "${HOST_IFACE}.${vid}" up
done

echo "=== Host VLAN interfaces ==="
ip link show | grep "${HOST_IFACE}\." | grep -v "link/"
