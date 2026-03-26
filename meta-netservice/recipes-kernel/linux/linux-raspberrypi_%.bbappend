# Apply netservice kernel config fragment on top of the RPi BSP defconfig.
# The % wildcard matches any linux-raspberrypi version in Scarthgap.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://netservice.cfg"
