# The recipe rpi-config writes config.txt for the boot partition. At the end it appends the
# variable RPI_EXTRA_CONFIG through printf, so a \n in the value becomes a new line.
#
# This asks the firmware to apply my overlay (overlays/stm32-node.dtbo in the boot partition)
# on top of the Pi's device tree at every boot: it adds the STM32 as a device on SPI chip
# select 0 and turns the spidev node there off (Phase 4, step 4.3).

# The recipe adds `dtoverlay=vc4-fkms-v3d` (the GPU driver) when VC4GRAPHICS is 1. That overlay
# also sets the CMA pool (memory reserved for devices that need one unbroken block) to 256 MiB
# (its fragment@0 writes size = 0x10000000), while the base device tree of the Zero 2 W has 64
# MiB. The gateway has no display and no camera, so I turn the overlay off.
# VC4GRAPHICS is a variable of this recipe only, derived from MACHINE_FEATURES. I do not use
# DISABLE_VC4GRAPHICS, which removes the machine feature for every recipe and made `cairo` fail
# to build (ADR-0017). Expected on the board: the CMA pool is 64 MiB and the boot log shows about
# 192 MiB more available memory.
VC4GRAPHICS = "0"

RPI_EXTRA_CONFIG:append = "\n# STM32 sensor node on SPI chip select 0 (meta-gateway)\ndtoverlay=stm32-node"
