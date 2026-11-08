SUMMARY = "Image of the sensor gateway: only the packages the gateway needs"

LICENSE = "MIT"

# core-image gives the image tasks (do_rootfs, do_image_wic) and the table that turns an
# IMAGE_FEATURES word (for example ssh-server-openssh) into packages. Its default package
# list is long (Bluetooth, audio, every kernel module), so IMAGE_INSTALL below replaces it.
# How the list grew and which builds failed on the way: ADR-0017.
inherit core-image

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
    mosquitto \
    mosquitto-clients \
    gateway-publisher \
    "
# Why these entries:
# - Wi-Fi: the drivers are kernel MODULES in this kernel, so they are listed by name.
#   brcmfmac is the driver; brcmutil and cfg80211 are what it needs; only the -cyw vendor
#   module is used on this chip (seen with lsmod). wireless-regdb-static is needed by
#   country=GR in wpa_supplicant.conf.
# - Both firmware packages: the name the driver asks for (43430b0) is only a symlink in
#   ...bcm43430, and the real file is in ...bcm43436. Without both, wlan0 does not exist.
# - SPI: spi-bcm2835 is the driver of the Pi's SPI peripheral, spidev makes /dev/spidev0.0.
#   spi-frame-test is my bring-up tool (this layer).
# - stm32-node is the RECIPE of my kernel module, not kernel-module-stm32-node: that name
#   fails in do_rootfs, because the `module` class makes the recipe's own package an empty
#   one that depends on the real module package.
# - mosquitto is the broker (loopback only, config in mosquitto_%.bbappend);
#   mosquitto-clients gives mosquitto_pub/sub to check it (ADR-0020, ADR-0021).
# - gateway-publisher is my C program (recipes-app/): frames from /dev/stm32_sensor_node
#   to the broker.

# Packages that something only RECOMMENDS and that I do not want. A hard dependency
# (RDEPENDS) cannot be removed this way; the build fails and names the package.
#
# kernel-image is recommended by kernel-base and would put a 26 MB copy of the kernel in
# /boot of the root partition. It is never used: the firmware loads the kernel from
# partition 1. Kernel packages are renamed with the version, so the entry is a pattern.
BAD_RECOMMENDATIONS += "kernel-image-*"

# Language files (locale-base-*) were asked for directly, so BAD_RECOMMENDATIONS cannot
# remove them. They come from IMAGE_LINGUAS, which something in my configuration sets (not
# found); setting it empty here wins. The console and logs are English with the C locale.
IMAGE_LINGUAS = ""

# ssh-server-openssh: the SSH server (ADR-0015). The first build produced 0-byte sshd and
# ssh-keygen; `bitbake -f -c install openssh` fixed it. Check the rootfs for zero-byte files.
# NOT listed on purpose: empty-root-password, allow-empty-password (root with no password)
# and allow-root-login. With a key login, OpenSSH's default "prohibit-password" is enough:
# root can log in over SSH with a key and never with a password.
IMAGE_FEATURES += "ssh-server-openssh"

# The device-tree overlay goes into the FAT boot partition as
# overlays/stm32-node.dtbo. IMAGE_BOOT_FILES is the list of files `wic` copies there, each as
# `<file in the deploy folder>;<destination>`. The second line makes the image wait for the
# recipe stm32-node-overlay. The `dtoverlay=stm32-node` line in config.txt comes from
# rpi-config_%.bbappend.
IMAGE_BOOT_FILES:append = " devicetree/stm32-node.dtbo;overlays/stm32-node.dtbo"
do_image_wic[depends] += "stm32-node-overlay:do_deploy"

# The password of root (ADR-0015). Only the SHA-512 hash (`openssl passwd -6`) is in the
# image, read from a file OUTSIDE the repo, like the Wi-Fi key: ${TOPDIR}/../secrets/root.hash
# (~/yocto/secrets next to ~/yocto/build). It is written into /etc/shadow after the root
# filesystem is built, so it is not a variable: it stays out of the task hash, `bitbake -e`
# and the run script. Only the PATH is part of the task hash. Do not build with `bitbake -v`:
# the verbose log may print the commands.
#
# Unlike the Wi-Fi .bbappend, this stops the build if the file is missing or wrong: an image
# without a root password is locked, even on the serial console.
set_root_password() {
    hashfile=${TOPDIR}/../secrets/root.hash

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

# SSH login for root with a key, and no password login over SSH (ADR-0015). The file is the
# PUBLIC key (the private half stays on my PC), so it lives in the repo. The serial console
# still asks for the root password, as a way in if the key does not work.
#
# sshd ignores authorized_keys if someone else can write the file or its folders, so the
# modes are 700 (folder) and 600 (file). This runs under pseudo, so the owner is root.
#
# PasswordAuthentication is a commented-out line in sshd_config, so sed replaces it, and the
# check after it stops the build if the line was not there.
install_root_ssh_key() {
    keyfile=${THISDIR}/files/root_authorized_keys

    if [ ! -s "$keyfile" ]; then
        bbfatal "install_root_ssh_key: $keyfile is missing or empty"
    fi
    case "$(cat "$keyfile")" in
        'ssh-ed25519 '*) ;;
        *) bbfatal "install_root_ssh_key: $keyfile does not start with ssh-ed25519" ;;
    esac

    install -d -m 0700 ${IMAGE_ROOTFS}/home/root/.ssh
    install -m 0600 "$keyfile" ${IMAGE_ROOTFS}/home/root/.ssh/authorized_keys

    sed -i 's/^#\?PasswordAuthentication .*/PasswordAuthentication no/' ${IMAGE_ROOTFS}/etc/ssh/sshd_config
    if ! grep -q '^PasswordAuthentication no$' ${IMAGE_ROOTFS}/etc/ssh/sshd_config; then
        bbfatal "install_root_ssh_key: PasswordAuthentication was not set in sshd_config"
    fi
}
ROOTFS_POSTPROCESS_COMMAND += "install_root_ssh_key;"
# A new key in the file must rebuild the root filesystem; the function body only has the path.
do_rootfs[file-checksums] += "${THISDIR}/files/root_authorized_keys:True"
