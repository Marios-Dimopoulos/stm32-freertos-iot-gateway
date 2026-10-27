# The recipe rpi-config writes config.txt for the boot partition. At the end it appends the
# variable RPI_EXTRA_CONFIG through printf, so a \n in the value becomes a new line.
#
# This asks the firmware to apply my overlay (overlays/stm32-node.dtbo in the boot partition)
# on top of the Pi's device tree at every boot: it adds the STM32 as a device on SPI chip
# select 0 and turns the spidev node there off (Phase 4, step 4.3).
RPI_EXTRA_CONFIG:append = "\n# STM32 sensor node on SPI chip select 0 (meta-gateway)\ndtoverlay=stm32-node"
