SUMMARY = "Device-tree overlay that adds the STM32 sensor node on SPI chip select 0"
DESCRIPTION = "Phase 4, step 4.3. Compiled from stm32-node.dts to stm32-node.dtbo and deployed \
next to the image; gateway-image.bb puts it into /boot/overlays of the boot partition and \
the file config.txt loads it with dtoverlay=stm32-node."

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# The class compiles every .dts in the source folder with `dtc` (the device-tree compiler). A
# file that has the line `/plugin/;` is an overlay, so the result is a .dtbo, and it is deployed
# to <deploy folder of the images>/devicetree/stm32-node.dtbo.
inherit devicetree

SRC_URI = "file://stm32-node.dts"

# The class refuses every machine by default.
COMPATIBLE_MACHINE = "raspberrypi0-2w-64"

# The class says this recipe provides "virtual/dtb", the base device tree of the machine. That
# is the kernel's job here, and two providers could clash; an overlay is not a base tree.
PROVIDES:remove = "virtual/dtb"

# The class adds the kernel's include folders and a dependency on the kernel configure step, for
# device trees that #include kernel headers. My overlay includes nothing.
KERNEL_INCLUDE = ""
