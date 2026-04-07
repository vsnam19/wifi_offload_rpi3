#!/bin/bash
# drop-mptcp-path.sh — Drop and restore a specific MPTCP subflow path
#
# Used during fota_consumer download to test MPTCP path resilience.
# Blocks traffic on a VLAN interface using iptables, waits, then restores.
#
# Usage:
#   sudo ./scripts/drop-mptcp-path.sh <vlan_id> [drop_seconds]
#
# Examples:
#   sudo ./scripts/drop-mptcp-path.sh 100      # drop eth0.100 path for 15s (default)
#   sudo ./scripts/drop-mptcp-path.sh 200 30   # drop eth0.200 path for 30s
#   sudo ./scripts/drop-mptcp-path.sh 300 10   # drop eth0.300 (lte_b2b) path for 10s
#
# What it does:
#   - Adds iptables FORWARD DROP rules on the host VLAN subinterface
#   - RPi's MPTCP TCP layer detects subflow stall and retransmits on remaining paths
#   - After <drop_seconds> the rule is removed and the subflow can rejoin
#
# VLAN → host interface mapping:
#   100 → enp2s0.100   (multipath, 172.16.1.0/24)
#   200 → enp2s0.200   (lte_b2c,   172.16.2.0/24)
#   300 → enp2s0.300   (lte_b2b,   172.16.3.0/24)

set -euo pipefail

HOST_IFACE="${HOST_IFACE:-enp2s0}"
VLAN_ID="${1:?Usage: $0 <vlan_id> [drop_seconds]}"
DROP_SECONDS="${2:-15}"

IFACE="${HOST_IFACE}.${VLAN_ID}"

# Validate vlan is configured
if ! ip link show "${IFACE}" > /dev/null 2>&1; then
    echo "[DROP] ERROR: interface ${IFACE} does not exist" >&2
    exit 1
fi

echo "[DROP] dropping path: iface=${IFACE} for ${DROP_SECONDS}s"
echo "[DROP] RPi MPTCP stack will retransmit on remaining subflows"

# ── Block both directions on this VLAN ───────────────────────────────────────
# INPUT drop: packets from RPi to host
# FORWARD drop: packets transiting host on this interface
iptables -I INPUT  1 -i "${IFACE}" -j DROP
iptables -I OUTPUT 1 -o "${IFACE}" -j DROP

echo "[DROP] rules added at $(date '+%H:%M:%S') — path is DOWN"
echo "[DROP] restoring in ${DROP_SECONDS}s ..."
sleep "${DROP_SECONDS}"

# ── Restore ───────────────────────────────────────────────────────────────────
iptables -D INPUT  -i "${IFACE}" -j DROP 2>/dev/null || true
iptables -D OUTPUT -o "${IFACE}" -j DROP 2>/dev/null || true

echo "[DROP] rules removed at $(date '+%H:%M:%S') — path restored"
echo "[DROP] done"
