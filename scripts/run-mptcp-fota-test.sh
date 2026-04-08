#!/bin/bash
# run-mptcp-fota-test.sh — Full MPTCP FOTA download test
#
# Orchestrates the complete test scenario:
#   1. Host: generate 1.2 GB firmware image, configure MPTCP endpoints, start server
#   2. RPi:  configure eth0.300, add all 3 MPTCP subflow endpoints, deploy fota_consumer
#   3. RPi:  start fota_consumer download in background
#   4. Host: after 10s drop path VLAN 100 for 15s (primary path disruption)
#   5. Host: after 40s drop path VLAN 200 for 15s (secondary path disruption)
#   6. Wait for download to complete, collect result
#   7. Write test report to /tmp/mptcp-fota-result.txt
#
# Usage:
#   sudo ./scripts/run-mptcp-fota-test.sh
#
# Requirements (host):
#   - VLANs up: enp2s0.100/200/300 with 172.16.{1,2,3}.1/24
#   - RPi reachable on 172.16.1.2
#   - python3, iptables
#
# Requirements (RPi):
#   - Kernel MPTCP enabled (net.mptcp.enabled=1)
#   - fota_consumer binary at /usr/sbin/fota_consumer

set -euo pipefail

HOST_IFACE="${HOST_IFACE:-enp2s0}"
RPI_SSH="${RPI_SSH:-root@172.16.1.2}"
SERVER_IP="${SERVER_IP:-172.16.1.1}"
SERVER_PORT="${SERVER_PORT:-8080}"

# SSH/SCP must run as the invoking user — root (sudo) has no RPi keys.
# SUDO_USER is set by sudo; fall back to $USER when run without sudo.
_SSHUSER="${SUDO_USER:-$USER}"
_SSH="sudo -u ${_SSHUSER} ssh -o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=no"
_SCP="sudo -u ${_SSHUSER} scp -o BatchMode=yes -o StrictHostKeyChecking=no"
SIZE_GB="${SIZE_GB:-1.2}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="/tmp/mptcp-fota-test-$(date +%Y%m%d-%H%M%S)"
RESULT_FILE="${LOG_DIR}/result.txt"

mkdir -p "${LOG_DIR}"

# ── colour output ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()    { echo -e "${GREEN}[TEST] $*${NC}";  }
warn()    { echo -e "${YELLOW}[WARN] $*${NC}"; }
err()     { echo -e "${RED}[FAIL] $*${NC}" >&2; }

# ── cleanup on exit ───────────────────────────────────────────────────────────
cleanup() {
    info "cleanup: removing iptables rules …"
    iptables -D INPUT  -i "${HOST_IFACE}.100" -j DROP 2>/dev/null || true
    iptables -D OUTPUT -o "${HOST_IFACE}.100" -j DROP 2>/dev/null || true
    iptables -D INPUT  -i "${HOST_IFACE}.200" -j DROP 2>/dev/null || true
    iptables -D OUTPUT -o "${HOST_IFACE}.200" -j DROP 2>/dev/null || true
    iptables -D INPUT  -i "${HOST_IFACE}.300" -j DROP 2>/dev/null || true
    iptables -D OUTPUT -o "${HOST_IFACE}.300" -j DROP 2>/dev/null || true
    info "cleanup: killing FOTA server …"
    kill "${SERVER_PID:-0}" 2>/dev/null || true
    ip mptcp endpoint flush 2>/dev/null || true
}
trap cleanup EXIT

# ─────────────────────────────────────────────────────────────────────────────
# STEP 0: Preflight checks
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 0: preflight checks ==="

for vid in 100 200; do
    iface="${HOST_IFACE}.${vid}"
    if ! ip link show "${iface}" > /dev/null 2>&1; then
        err "host interface ${iface} not found — run scripts/setup-test-vlans.sh first"
        exit 1
    fi
done

if ! ${_SSH} "${RPI_SSH}" "echo ok" > /dev/null 2>&1; then
    err "RPi unreachable at ${RPI_SSH}"
    exit 1
fi

if ! ${_SSH} "${RPI_SSH}" "test -x /usr/sbin/fota_consumer" 2>/dev/null; then
    warn "fota_consumer not on RPi — deploying from /tmp/fota_consumer_arm …"
    if [[ ! -f /tmp/fota_consumer_arm ]]; then
        err "/tmp/fota_consumer_arm not found — run the cross-compile step first"
        exit 1
    fi
    ${_SCP} /tmp/fota_consumer_arm "${RPI_SSH}:/usr/sbin/fota_consumer"
    ${_SSH} "${RPI_SSH}" "chmod 755 /usr/sbin/fota_consumer"
    info "fota_consumer deployed to RPi"
fi

MPTCP_HOST=$(cat /proc/sys/net/mptcp/enabled 2>/dev/null || echo 0)
MPTCP_RPI=$(${_SSH} "${RPI_SSH}" "cat /proc/sys/net/mptcp/enabled 2>/dev/null || echo 0")
info "MPTCP: host=${MPTCP_HOST}  rpi=${MPTCP_RPI}"
if [[ "${MPTCP_HOST}" != "1" || "${MPTCP_RPI}" != "1" ]]; then
    err "MPTCP not enabled on both sides"
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# STEP 1: RPi — add eth0.300 + MPTCP endpoints
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 1: configuring RPi MPTCP endpoints ==="

${_SSH} "${RPI_SSH}" '
set -e
modprobe 8021q 2>/dev/null || true

# Add eth0.300 if missing
if ! ip link show eth0.300 > /dev/null 2>&1; then
    ip link add link eth0 name eth0.300 type vlan id 300
    ip link set eth0.300 up
    ip addr add 172.16.3.2/24 dev eth0.300
    echo "eth0.300 created: 172.16.3.2/24"
else
    echo "eth0.300 already exists"
fi

# Flush + re-add MPTCP subflow endpoints (endpoints 1&2 may already exist)
ip mptcp endpoint flush 2>/dev/null || true
ip mptcp endpoint add 172.16.1.2 id 1 subflow dev eth0.100
ip mptcp endpoint add 172.16.2.2 id 2 subflow dev eth0.200
ip mptcp endpoint add 172.16.3.2 id 3 subflow dev eth0.300
ip mptcp limits set add_addr_accepted 3 subflows 3
echo "=== RPi MPTCP endpoints ==="
ip mptcp endpoint show
ip mptcp limits show
'

# ─────────────────────────────────────────────────────────────────────────────
# STEP 2: Host — add MPTCP signal endpoints + start server
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 2: configuring host MPTCP endpoints ==="

ip mptcp endpoint flush 2>/dev/null || true
ip mptcp endpoint add 172.16.1.1 id 1 signal
ip mptcp endpoint add 172.16.2.1 id 2 signal
ip mptcp endpoint add 172.16.3.1 id 3 signal
ip mptcp limits set add_addr_accepted 3 subflows 3
ip mptcp endpoint show

info "=== STEP 3: starting FOTA HTTP server ==="
python3 "${SCRIPT_DIR}/mptcp_server.py" \
    --port "${SERVER_PORT}" \
    --size-gb "${SIZE_GB}" \
    --no-endpoints \
    > "${LOG_DIR}/server.log" 2>&1 &
SERVER_PID=$!
info "server PID=${SERVER_PID}, log=${LOG_DIR}/server.log"

# Start ip mptcp monitor on host — records subflow add/remove events
ip mptcp monitor > "${LOG_DIR}/mptcp_monitor_host.log" 2>&1 &
MPTCP_MONITOR_PID=$!
info "ip mptcp monitor PID=${MPTCP_MONITOR_PID}"

sleep 3   # wait for firmware image to be created + server to bind

# Get SHA-256 from server
EXPECTED_SHA=$(curl -sf "http://${SERVER_IP}:${SERVER_PORT}/firmware.img.sha256" 2>/dev/null || echo "")
info "firmware SHA-256 (head+tail): ${EXPECTED_SHA:-<unavailable>}"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 3: RPi — start fota_consumer download in background
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 4: starting fota_consumer on RPi ==="
T_START=$(date +%s)

${_SSH} "${RPI_SSH}" "
nohup fota_consumer \
    --server ${SERVER_IP} \
    --port   ${SERVER_PORT} \
    --path   /firmware.img \
    --out    /tmp/firmware_downloaded.img \
    > /tmp/fota_consumer.log 2>&1 &
echo \$!
" > "${LOG_DIR}/rpi_pid.txt"

RPI_PID=$(cat "${LOG_DIR}/rpi_pid.txt" | tail -1)
info "fota_consumer started on RPi PID=${RPI_PID}"

# Start periodic evidence collector: every 5s snapshot ss --mptcp + /proc/net/mptcp on RPi
{
    snap=0
    while true; do
        sleep 5
        snap=$(( snap + 1 ))
        TS=$(date '+%H:%M:%S')
        {
            echo ""
            echo "=== snapshot #${snap}  ${TS} ==="
            echo "--- ss --mptcp -i (RPi) ---"
            ${_SSH} "${RPI_SSH}" "ss --mptcp -i 2>/dev/null || ss -t -i 2>/dev/null | head -30" 2>/dev/null
            echo "--- /proc/net/mptcp (RPi) ---"
            ${_SSH} "${RPI_SSH}" "cat /proc/net/mptcp 2>/dev/null || echo 'N/A'" 2>/dev/null
            echo "--- ip mptcp endpoint (RPi) ---"
            ${_SSH} "${RPI_SSH}" "ip mptcp endpoint show 2>/dev/null" 2>/dev/null
        } >> "${LOG_DIR}/mptcp_snapshots.log" 2>/dev/null
        # Stop if download done (check by looking for SUCCESS/INCOMPLETE in log)
        ${_SSH} "${RPI_SSH}" "grep -q 'SUCCESS\|INCOMPLETE\|SUMMARY' /tmp/fota_consumer.log 2>/dev/null" 2>/dev/null && break
    done
} &
SNAPSHOT_PID=$!

# ─────────────────────────────────────────────────────────────────────────────
# STEP 4: Drop VLAN 100 (primary path) at T+10s
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 5: waiting 10s before first path drop ==="
sleep 10

warn ">>> DROPPING path VLAN 100 (${HOST_IFACE}.100 — primary multipath link) for 15s <<<"
T_DROP1=$(date +%s)
echo "[$(date '+%H:%M:%S')] DROP VLAN 100" >> "${LOG_DIR}/events.txt"
iptables -I INPUT  1 -i "${HOST_IFACE}.100" -j DROP
iptables -I OUTPUT 1 -o "${HOST_IFACE}.100" -j DROP
sleep 15

warn ">>> RESTORING path VLAN 100 <<<"
iptables -D INPUT  -i "${HOST_IFACE}.100" -j DROP
iptables -D OUTPUT -o "${HOST_IFACE}.100" -j DROP
T_RESTORE1=$(date +%s)
echo "[$(date '+%H:%M:%S')] RESTORE VLAN 100  (dropped for $((T_RESTORE1 - T_DROP1))s)" >> "${LOG_DIR}/events.txt"
info "path VLAN 100 restored after $((T_RESTORE1 - T_DROP1))s"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 5: Drop VLAN 200 (secondary path) at T+40s
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 6: waiting 15s before second path drop ==="
sleep 15

warn ">>> DROPPING path VLAN 200 (${HOST_IFACE}.200 — lte_b2c link) for 15s <<<"
T_DROP2=$(date +%s)
echo "[$(date '+%H:%M:%S')] DROP VLAN 200" >> "${LOG_DIR}/events.txt"
iptables -I INPUT  1 -i "${HOST_IFACE}.200" -j DROP
iptables -I OUTPUT 1 -o "${HOST_IFACE}.200" -j DROP
sleep 15

warn ">>> RESTORING path VLAN 200 <<<"
iptables -D INPUT  -i "${HOST_IFACE}.200" -j DROP
iptables -D OUTPUT -o "${HOST_IFACE}.200" -j DROP
T_RESTORE2=$(date +%s)
echo "[$(date '+%H:%M:%S')] RESTORE VLAN 200  (dropped for $((T_RESTORE2 - T_DROP2))s)" >> "${LOG_DIR}/events.txt"
info "path VLAN 200 restored after $((T_RESTORE2 - T_DROP2))s"

# ─────────────────────────────────────────────────────────────────────────────
# STEP 6: Wait for download to complete (max 10 minutes)
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 7: waiting for download to complete (max 600s) ==="
DEADLINE=$(( $(date +%s) + 600 ))
while (( $(date +%s) < DEADLINE )); do
    if ${_SSH} "${RPI_SSH}" "test -f /tmp/fota_consumer.log && grep -q 'SUCCESS\|INCOMPLETE\|MISMATCH' /tmp/fota_consumer.log" 2>/dev/null; then
        break
    fi
    sleep 5
    # Print progress line from RPi log
    PROGRESS=$(${_SSH} "${RPI_SSH}" "tail -1 /tmp/fota_consumer.log 2>/dev/null" || echo "")
    if [[ -n "${PROGRESS}" ]]; then
        echo -e "  ${PROGRESS}"
    fi
done

T_END=$(date +%s)
ELAPSED=$(( T_END - T_START ))

kill "${SNAPSHOT_PID}" 2>/dev/null || true
kill "${MPTCP_MONITOR_PID}" 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# STEP 7: Collect results
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 8: collecting results ==="

# Collect fota_consumer log
${_SCP} "${RPI_SSH}:/tmp/fota_consumer.log" "${LOG_DIR}/fota_consumer.log" 2>/dev/null || true

# File size on RPi
DOWNLOADED_SIZE=$(${_SSH} "${RPI_SSH}" "stat -c%s /tmp/firmware_downloaded.img 2>/dev/null || echo 0")
EXPECTED_SIZE=$(${_SSH} "${RPI_SSH}" "cat /proc/\$(cat /tmp/fota_consumer.log | grep -o 'received=[0-9]*' | tail -1 | cut -d= -f2) 2>/dev/null || echo ''" || echo "")

# Check success
CONSUMER_EXIT=$(${_SSH} "${RPI_SSH}" "grep -c 'SUCCESS' /tmp/fota_consumer.log 2>/dev/null || echo 0")
CONSUMER_INCOMPLETE=$(${_SSH} "${RPI_SSH}" "grep -c 'INCOMPLETE' /tmp/fota_consumer.log 2>/dev/null || echo 0")

# Collect server log
head -50 "${LOG_DIR}/server.log" > "${LOG_DIR}/server_excerpt.log" 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# STEP 8: Write result report
# ─────────────────────────────────────────────────────────────────────────────
info "=== STEP 9: writing result report → ${RESULT_FILE} ==="

{
echo "================================================================"
echo "  MPTCP FOTA Download Test — $(date '+%Y-%m-%d %H:%M:%S')"
echo "================================================================"
echo ""
echo "Target:     RPi3 (${RPI_SSH})"
echo "Server:     ${SERVER_IP}:${SERVER_PORT}"
echo "File size:  ${SIZE_GB} GB"
echo "Total time: ${ELAPSED}s"
echo "SHA-256:    ${EXPECTED_SHA:-N/A}"
echo ""
echo "---- Path Drop Events ----"
cat "${LOG_DIR}/events.txt" 2>/dev/null || echo "(none recorded)"
echo ""
echo "---- Download Result ----"
echo "File on RPi: /tmp/firmware_downloaded.img (${DOWNLOADED_SIZE} bytes)"
if [[ "${CONSUMER_EXIT}" -gt 0 ]]; then
    echo "RESULT: SUCCESS — download completed despite path drops"
elif [[ "${CONSUMER_INCOMPLETE}" -gt 0 ]]; then
    echo "RESULT: INCOMPLETE — download did not finish"
else
    echo "RESULT: UNKNOWN — check ${LOG_DIR}/fota_consumer.log"
fi
echo ""
echo "---- Throughput Samples ([TPUT] lines from fota_consumer) ----"
grep -E '^\[TPUT\]' "${LOG_DIR}/fota_consumer.log" 2>/dev/null | head -80 || echo "(no samples)"
echo ""
echo "---- Path Events ([FOTA] PATH EVENT lines) ----"
grep -E 'PATH EVENT|subflow disruption|path restored|DOWNLOAD SUMMARY|MPTCP sub' \
    "${LOG_DIR}/fota_consumer.log" 2>/dev/null || echo "(no path events)"
echo ""
echo "---- MPTCP Subflow Monitor Events (host) ----"
cat "${LOG_DIR}/mptcp_monitor_host.log" 2>/dev/null | head -60 || echo "(no mptcp monitor output)"
echo ""
echo "---- MPTCP Snapshots (ss --mptcp + /proc/net/mptcp from RPi) ----"
cat "${LOG_DIR}/mptcp_snapshots.log" 2>/dev/null | head -200 || echo "(no snapshots)"
echo ""
echo "---- fota_consumer log (last 40 lines) ----"
tail -40 "${LOG_DIR}/fota_consumer.log" 2>/dev/null || echo "(no log)"
echo ""
echo "---- Server log (last 10 lines) ----"
tail -10 "${LOG_DIR}/server.log" 2>/dev/null || echo "(no log)"
echo "================================================================"
} | tee "${RESULT_FILE}"

info "Test complete. Logs in ${LOG_DIR}/"
info "Result file: ${RESULT_FILE}"

if [[ "${CONSUMER_EXIT}" -gt 0 ]]; then
    exit 0
else
    exit 1
fi
