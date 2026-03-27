# Apply netservice kernel config fragment to linux-yocto (used by qemuarm machine).
# For linux-raspberrypi (rpi3 machine), see linux-raspberrypi_%.bbappend.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://netservice.cfg"
