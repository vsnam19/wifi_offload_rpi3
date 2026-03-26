SUMMARY = "WiFi Offload Manager daemon"
DESCRIPTION = "Network Infrastructure Service: manages multi-path connectivity \
               (WiFi + LTE) using MPTCP and cgroup-based traffic isolation."
LICENSE = "CLOSED"

# ── Source ────────────────────────────────────────────────────────
# wifi-offload-manager/ lives in the same repo as this layer.
# devtool modify wifi-offload-manager for fast iterative development.
S = "${WORKDIR}/wifi-offload-manager"

SRC_URI = " \
    file://wifi-offload-manager \
    file://wifi-offload-manager.service \
    file://path-policies.json \
"

# ── Dependencies ──────────────────────────────────────────────────
DEPENDS = " \
    libmnl \
    nlohmann-json \
"

RDEPENDS:${PN} = " \
    libmnl \
    wpa-supplicant \
    iproute2 \
    iptables \
"

# ── Build ─────────────────────────────────────────────────────────
inherit cmake systemd

EXTRA_OECMAKE = " \
    -DCMAKE_BUILD_TYPE=Release \
"

# ── systemd integration ───────────────────────────────────────────
SYSTEMD_SERVICE:${PN} = "wifi-offload-manager.service"
SYSTEMD_AUTO_ENABLE = "enable"

# ── Install ───────────────────────────────────────────────────────
do_install:append() {
    # Default config → /etc/netservice/
    install -d ${D}${sysconfdir}/netservice
    install -m 0644 ${WORKDIR}/path-policies.json \
        ${D}${sysconfdir}/netservice/path-policies.json

    # Runtime socket directory
    install -d ${D}${localstatedir}/run/netservice
}

FILES:${PN} += " \
    ${sysconfdir}/netservice/ \
    ${localstatedir}/run/netservice/ \
"
