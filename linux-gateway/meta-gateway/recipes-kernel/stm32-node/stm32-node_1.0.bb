SUMMARY = "Kernel driver for the STM32 sensor node on the SPI bus (Phase 4)"
DESCRIPTION = "SPI driver for the STM32 node. It binds to the device-tree node with the compatible \
string mdimopoulos,stm32-sensor-node and creates /dev/stm32_sensor_node. Each read() does an SPI \
transfer, checks the marker and the CRC of the frame and returns its 14 raw bytes, or -EIO if the \
frame is bad (ADR-0018)."

# A kernel module is part of the kernel when it runs, so it must be GPL. The license text is the
# one that ships with Yocto (same checksum as in make-mod-scripts_1.0.bb).
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# `module` knows how to build against the kernel that Yocto built (it passes the kernel source
# and build folders to `make` through KERNEL_SRC and O=), and installs the .ko under
# /lib/modules/<kernel version>/updates (checked on the first build, 2026-10-25; the file is
# compressed, stm32_node.ko.xz, like the other modules of this kernel). It also splits the result
# into a package called kernel-module-stm32-node-<kernel version>. The package ${PN} itself is
# empty and only depends on that one.
inherit module

SRC_URI = "file://Makefile \
           file://stm32_node.c"

# No archive to extract, so the sources are in ${UNPACKDIR}. bitbake.conf sets B = "${S}", so
# `make` runs in the same folder as the files.
S = "${UNPACKDIR}"
