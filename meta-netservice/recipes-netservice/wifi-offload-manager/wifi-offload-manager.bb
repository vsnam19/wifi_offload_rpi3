SUMMARY = "WiFi Offload Manager daemon"
DESCRIPTION = "Network Infrastructure Service: manages multi-path connectivity \
               (WiFi + LTE) using MPTCP and cgroup-based traffic isolation."
LICENSE = "CLOSED"

# ── Source ────────────────────────────────────────────────────────
# wifi-offload-manager/ lives at the project root (one level above the
# kas build directory).  FILESEXTRAPATHS:prepend lets BitBake find it
# when resolving the file:// URIs below.
FILESEXTRAPATHS:prepend := "${TOPDIR}/../:"

S = "${WORKDIR}/wifi-offload-manager"

SRC_URI = " \
    file://wifi-offload-manager \
    file://wifi-offload-manager.service \
    file://path-policies.json \
"

# ── Dependencies ──────────────────────────────────────────────────
DEPENDS = " \
    libmnl \
    iptables \
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
    -DBUILD_TESTS=OFF \
"

# ── systemd integration ───────────────────────────────────────────
SYSTEMD_SERVICE:${PN} = "wifi-offload-manager.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# ── Install ───────────────────────────────────────────────────────
do_install:append() {
    # systemd service unit → /lib/systemd/system/
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/wifi-offload-manager.service \
        ${D}${systemd_system_unitdir}/wifi-offload-manager.service

    # Default config → /etc/netservice/
    install -d ${D}${sysconfdir}/netservice
    install -m 0644 ${WORKDIR}/path-policies.json \
        ${D}${sysconfdir}/netservice/path-policies.json
    # Note: /run/netservice/ is created at runtime by RuntimeDirectory=netservice
    # in the systemd unit — do NOT install it here (usrmerge: /var/run → /run)
}

FILES:${PN} += " \
    ${sysconfdir}/netservice/ \
"
