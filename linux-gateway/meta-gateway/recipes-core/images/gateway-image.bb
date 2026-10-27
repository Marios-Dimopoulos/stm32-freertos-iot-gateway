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
    stm32-node \
    "
# Phase 4, step 4.2: my own kernel module (recipe stm32-node in this layer). For now it only
# prints a line when it is loaded and unloaded.
# The name on the list is the RECIPE, not kernel-module-stm32-node: with that name do_rootfs
# failed ("No match for argument: kernel-module-stm32-node", "neither a recipe nor a generated
# package"), although the package data does list it as an alias of the real package
# kernel-module-stm32-node-6.18.33-v8. The `module` class makes the package of the recipe an
# empty one that only depends on the real module package, and that is the name to list. (Why
# the alias fails for my module and works for spidev, I do not know.)

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

# Features of this image. They were in local.conf until 2026-10-24, where they applied to every
# image.
#   ssh-server-openssh  the SSH server (ADR-0015). Note: the first build with it produced 0-byte
#                       sshd, ssh-keygen and other programs (a race while `make install` ran in
#                       parallel, as far as I can tell); `bitbake -f -c install openssh` gave
#                       complete files. Check the new rootfs for zero-byte files.
#   allow-root-login    PermitRootLogin yes in sshd_config. Without it OpenSSH refuses root with
#                       a password ("prohibit-password"), and I log in as root.
# NOT listed on purpose: empty-root-password and allow-empty-password. They made root log in
# with no password, on the serial console and over SSH.
IMAGE_FEATURES += "ssh-server-openssh allow-root-login"

# Phase 4, step 4.3: the device-tree overlay of the STM32 node goes into the boot partition (the
# FAT one) as overlays/stm32-node.dtbo. IMAGE_BOOT_FILES is the list of files that `wic` copies
# into that partition, each as `<file in the deploy folder>;<destination>`. The recipe
# stm32-node-overlay deploys the file; the second line makes the image wait for it. The
# `dtoverlay=stm32-node` line in config.txt comes from rpi-config_%.bbappend in this layer.
IMAGE_BOOT_FILES:append = " devicetree/stm32-node.dtbo;overlays/stm32-node.dtbo"
do_image_wic[depends] += "stm32-node-overlay:do_deploy"

# The password of root (follow-up of ADR-0015). Only the hash is in the image (SHA-512 crypt, from
# `openssl passwd -6`), and the hash is in a file OUTSIDE the repo, like the Wi-Fi key. It is
# written into /etc/shadow here, after the root filesystem is built, so that it is not a
# variable: it does not go into the task hash, `bitbake -e` or the generated run script.
# (Do not build with `bitbake -v`: the verbose log may print the commands.)
#
# Unlike the Wi-Fi .bbappend, this stops the build if the file is missing or wrong, because an
# image without a root password is locked (root cannot log in on the serial console either).
set_root_password() {
    hashfile=/home/mariosdimopoulos/yocto/secrets/root.hash

    if [ ! -s "$hashfile" ]; then
        bbfatal "set_root_password: $hashfile is missing or empty (see linux-gateway/README.md)"
    fi
    hash=$(cat "$hashfile")
    case "$hash" in
        '$6$'*) ;;
        *) bbfatal "set_root_password: $hashfile does not start with the SHA-512 crypt marker" ;;
    esac

    # The characters of such a hash are letters, digits, '.', '/' and '$': none of them is
    # special in the replacement text of sed or the delimiter '|'.
    sed -i "s|^root:[^:]*:|root:$hash:|" ${IMAGE_ROOTFS}/etc/shadow

    if ! grep -qF "root:$hash:" ${IMAGE_ROOTFS}/etc/shadow; then
        bbfatal "set_root_password: the root line of /etc/shadow was not changed"
    fi
}
ROOTFS_POSTPROCESS_COMMAND += "set_root_password;"
