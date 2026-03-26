SUMMARY = "WiFi Offload Manager — minimal headless image for RPi3B+ prototype"
DESCRIPTION = "Boots the wifi-offload-manager daemon with wpa_supplicant, \
               iproute2, iptables, and kernel MPTCP support."

# Inherit core image class from poky
inherit core-image

# ── Base image features ───────────────────────────────────────────
IMAGE_FEATURES += "ssh-server-openssh"

# ── Packages ──────────────────────────────────────────────────────
IMAGE_INSTALL:append = " \
    wifi-offload-manager \
    wpa-supplicant \
    iproute2 \
    iproute2-tc \
    iptables \
    kmod \
    kernel-modules \
    libmnl \
    nlohmann-json \
"
# Note: openssh is installed via IMAGE_FEATURES += ssh-server-openssh above.
# Note: systemd is selected as init manager via VIRTUAL-RUNTIME_init_manager
#       in local_conf_header — do NOT add it to IMAGE_INSTALL.

# ── Image size ────────────────────────────────────────────────────
# 256 MB rootfs is ample for a headless daemon image
IMAGE_ROOTFS_SIZE ?= "262144"
IMAGE_OVERHEAD_FACTOR ?= "1.3"
