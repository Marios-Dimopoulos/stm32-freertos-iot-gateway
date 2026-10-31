SUMMARY = "Reads frames from the STM32 sensor node and publishes them to the MQTT broker"
DESCRIPTION = "Step 4.6c (ADR-0020, ADR-0021). Reads /dev/stm32_sensor_node, decodes the frame \
and sends it as JSON to the Mosquitto broker on the gateway."

# The program is my own code; same license and check as spi-frame-test.
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# `file://` means "look in the folders of the search path" (the `files/` folder next to this
# recipe is one of them). do_unpack copies the file to ${UNPACKDIR}, and S points there
# because there is no archive to extract.
SRC_URI = "file://gateway-publisher.c"
S = "${UNPACKDIR}"

# ${CC} is the cross-compiler already set up for the target's sysroot. ${CFLAGS} and ${LDFLAGS}
# are Yocto's standard flags; leaving them out would also skip the quality checks on the result.
do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} ${S}/gateway-publisher.c -o gateway-publisher
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 gateway-publisher ${D}${bindir}/gateway-publisher
}
