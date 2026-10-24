SUMMARY = "Image of the sensor gateway: only the packages the gateway needs"

LICENSE = "MIT"

# core-image gives the image tasks (do_rootfs, do_image_wic) and the table that turns an
# IMAGE_FEATURES word (for example ssh-server-openssh) into packages. Its default package
# list, which is long (packagegroup-base-extended pulls in Bluetooth, audio, graphics
# libraries and every kernel module), is replaced below on purpose.
inherit core-image

# The image was built up one step at a time, and each step was built, flashed and tested
# before the next, so that a failure had one cause.
#
# Step 1 (tested): packagegroup-core-boot alone booted to a login on the serial console, 47
# packages, 29 MB root filesystem.
#
# Step 2 (tested): Wi-Fi. wpa-supplicant joins the network, iw shows it, wireless-regdb-static
# is the regulatory database (country=GR in wpa_supplicant.conf needs it), and the rest is the
# chip firmware and the drivers. The drivers are kernel MODULES in this kernel, so they are
# installed by name; the old image got them through kernel-modules (28 MB of everything).
# brcmfmac is the driver, brcmutil and cfg80211 are what it depends on. This kernel splits
# brcmfmac into a core and three vendor modules that the core loads by chip: lsmod on the
# board showed that only brcmfmac_cyw is used, so -wcc and -bca are not listed.
#
# The first test of step 2 failed: wlan0 did not exist. The driver loaded, but dmesg said
# "Direct firmware load for brcm/brcmfmac43430b0-sdio.raspberrypi,model-zero-2-w.bin failed
# with error -2" (file not found). The chip reports itself as BCM43430/2, so the driver asks
# for a 43430b0 name, and that name is only a symlink (to the 43436 firmware) which lives in
# the package linux-firmware-rpidistro-bcm43430, not in ...bcm43436 where the real file is.
# The old image had both packages. Both are listed, because the symlink is useless without
# the file.
#
# Step 3 (tested): SPI. In step 1 `ls /dev/spi*` found nothing, because the SPI controller
# driver and spidev are modules in this kernel and no module was installed. spi-bcm2835 is
# the driver of the controller on the Pi's SoC; spidev makes /dev/spidev0.0 from it;
# spi-frame-test is my own bring-up tool (recipe in this layer) that reads the frames of the
# STM32 through it.
IMAGE_INSTALL = "\
    packagegroup-core-boot \
    wpa-supplicant \
    iw \
    wireless-regdb-static \
    linux-firmware-rpidistro-bcm43430 \
    linux-firmware-rpidistro-bcm43436 \
    kernel-module-brcmfmac \
    kernel-module-brcmfmac-cyw \
    kernel-module-brcmutil \
    kernel-module-cfg80211 \
    kernel-module-spi-bcm2835 \
    kernel-module-spidev \
    spi-frame-test \
    "

# Packages that something only RECOMMENDS (RRECOMMENDS) and that I do not want. A hard
# dependency (RDEPENDS) cannot be removed this way, and the build then fails with the name of
# the package that needs it.
#
# kernel-image: every kernel module package depends on kernel-base, and kernel-base recommends
# kernel-image, which puts a 26 MB copy of the kernel in /boot of the ROOT partition. The
# firmware loads the kernel from partition 1 (kernel8.img) and partition 1 is then mounted over
# /boot, so that copy is never used.
#
# The first try used the name "kernel-image" and did nothing: the build accepted it without
# any error, but the manifest still had kernel-image-6.18.33-v8 and kernel-image-image-6.18.33-v8.
# Yocto renames kernel packages to include the kernel version, and the exclusion works on the
# final names. The entries are handed to `dnf -x`, which takes patterns, so a pattern avoids
# putting the version number here.
BAD_RECOMMENDATIONS += "kernel-image-*"

# Language files: the image asked for locale-base-c, locale-base-en-us and locale-base-en-gb
# directly (they were in the `dnf install` command of do_rootfs), so they are not
# recommendations and BAD_RECOMMENDATIONS fails the build for them. They come from
# IMAGE_LINGUAS, which the class leaves empty but something else in my configuration sets; I
# did not find what. Setting it empty here, in the recipe, wins over that. The console and
# the logs are in English with the C locale and need no language files.
IMAGE_LINGUAS = ""
