#!/usr/bin/env bash
# deploy.sh — build wifi-offload-manager IPK and push it to target via SSH
#
# Usage:
#   ./scripts/deploy.sh                      # build + deploy to default target
#   ./scripts/deploy.sh root@172.16.45.2     # explicit target
#   ./scripts/deploy.sh --build-only         # build IPK without deploying
#
# Requirements on host:
#   - ~/.local/bin/kas (or kas in PATH)
#   - SSH access to target as root (key-based recommended)
#
# Requirements on target:
#   - opkg installed (included in netservice-image from this recipe)

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
KAS="${KAS:-$(command -v kas 2>/dev/null || echo "${HOME}/.local/bin/kas")}"
KAS_CONFIG="${REPO_ROOT}/kas/kas.yml"
RECIPE="wifi-offload-manager"
TARGET="${1:-root@172.16.45.2}"
ARCH="cortexa7t2hf-neon-vfpv4"
IPK_DIR="${REPO_ROOT}/build/tmp/deploy/ipk/${ARCH}"

# ── Parse args ────────────────────────────────────────────────────
BUILD_ONLY=0
if [[ "${1:-}" == "--build-only" ]]; then
    BUILD_ONLY=1
    TARGET=""
fi

# ── Step 1: Build IPK ─────────────────────────────────────────────
echo "==> Building ${RECIPE} IPK..."
"${KAS}" shell "${KAS_CONFIG}" --command "bitbake ${RECIPE}"

# Find the freshest IPK
IPK_FILE=$(ls -t "${IPK_DIR}/${RECIPE}_"*.ipk 2>/dev/null | head -1)
if [[ -z "${IPK_FILE}" ]]; then
    echo "ERROR: no IPK found in ${IPK_DIR}" >&2
    exit 1
fi

echo "==> Built: $(basename "${IPK_FILE}")"

if [[ "${BUILD_ONLY}" -eq 1 ]]; then
    echo "==> --build-only: skipping deploy"
    exit 0
fi

# ── Step 2: Copy IPK to target ────────────────────────────────────
echo "==> Copying to ${TARGET}:/tmp/..."
scp -q "${IPK_FILE}" "${TARGET}:/tmp/"

REMOTE_IPK="/tmp/$(basename "${IPK_FILE}")"

# ── Step 3: Install and restart ───────────────────────────────────
echo "==> Installing on target..."
ssh "${TARGET}" sh << EOF
set -e
cp ${REMOTE_IPK} /tmp/wom_new
if command -v opkg > /dev/null 2>&1; then
    opkg install --force-reinstall ${REMOTE_IPK}
    rm -f ${REMOTE_IPK}
else
    # No opkg — extract binary from IPK directly
    # IPK is an ar archive; data.tar.* contains the files
    cd /tmp && mkdir -p wom_ipk_extract && cd wom_ipk_extract
    ar x ${REMOTE_IPK}
    tar -xf data.tar.* 2>/dev/null || tar -xzf data.tar.gz 2>/dev/null || true
    if [ -f usr/sbin/wifi-offload-manager ]; then
        systemctl stop wifi-offload-manager 2>/dev/null || true
        mv usr/sbin/wifi-offload-manager /usr/sbin/wifi-offload-manager
        chmod 755 /usr/sbin/wifi-offload-manager
    fi
    cd /tmp && rm -rf wom_ipk_extract
    rm -f ${REMOTE_IPK}
fi
systemctl daemon-reload
systemctl restart wifi-offload-manager
sleep 1
echo ""
echo "=== systemctl status ==="
systemctl status wifi-offload-manager --no-pager -l
echo ""
echo "=== journalctl (last 30 lines) ==="
journalctl -u wifi-offload-manager --no-pager -n 30
EOF

echo ""
echo "==> Deploy complete. Live log:"
echo "    ssh ${TARGET} journalctl -u wifi-offload-manager -f"
