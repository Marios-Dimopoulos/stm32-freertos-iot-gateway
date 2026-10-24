SUMMARY = "Reads and checks frames from the STM32 sensor node through /dev/spidev0.0"
DESCRIPTION = "Bring-up tool for the SPI link (ADR-0010) on the gateway image, which has no Python. \
It uses the generic spidev interface; the Phase 4 kernel driver replaces that interface."

# The program is my own code. The license text is the one that ships with Yocto; BitBake
# fails the build if the md5 does not match, which is how it detects a changed license.
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# `file://` means "look in the folders of the search path" (the `files/` folder next to this
# recipe is one of them). do_unpack copies the file to ${UNPACKDIR}, and S points there
# because there is no archive to extract.
SRC_URI = "file://spi-frame-test.c"
S = "${UNPACKDIR}"

# ${CC} is the cross-compiler (aarch64-poky-linux-gcc) already set up to use the target's
# sysroot, which holds the header <linux/spi/spidev.h>. ${CFLAGS} and ${LDFLAGS} are Yocto's
# standard flags; leaving them out would also skip the quality checks on the result.
do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} ${S}/spi-frame-test.c -o spi-frame-test
}

# ${D} is the install folder of this recipe; whatever is under it goes into the package.
# ${bindir} is /usr/bin on the target.
do_install() {
	install -d ${D}${bindir}
	install -m 0755 spi-frame-test ${D}${bindir}/spi-frame-test
}
