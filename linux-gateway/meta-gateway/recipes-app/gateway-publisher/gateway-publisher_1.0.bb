SUMMARY = "Reads frames from the STM32 sensor node and publishes them to the MQTT broker"
DESCRIPTION = "Reads /dev/stm32_sensor_node, decodes the frame \
and sends it as JSON to the Mosquitto broker on the gateway. Started at boot by an init script."

# The program is my own code; same license and check as spi-frame-test.
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# `file://` means "look in the folders of the search path" (the `files/` folder next to this
# recipe is one of them). do_unpack copies the files to ${UNPACKDIR}, and S points there
# because there is no archive to extract.
SRC_URI = "file://gateway-publisher.c \
           file://gateway-publisher.init \
"
S = "${UNPACKDIR}"

# The program uses the client library of Mosquitto. DEPENDS makes BitBake build the `mosquitto`
# recipe first and put its headers (mosquitto/libmosquitto.h) and libmosquitto.so in the sysroot
# that ${CC} uses. Nothing is needed in RDEPENDS: libmosquitto1 is already in the image, and Yocto
# adds the runtime dependency by itself because the program links against that library.
DEPENDS = "mosquitto"

# Starts the program at boot. The class `update-rc.d` reads the two variables below and makes the
# links in /etc/rc*.d when the package is installed into the image. This is how the Mosquitto
# recipe does it too. "defaults 40 20" means: start link S40 (after S30mosquitto, so the broker is
# started first) and stop link K20 (before K30mosquitto, so the program stops first).
inherit update-rc.d
INITSCRIPT_NAME = "gateway-publisher"
INITSCRIPT_PARAMS = "defaults 40 20"

# ${CC} is the cross-compiler already set up for the target's sysroot. ${CFLAGS} and ${LDFLAGS}
# are Yocto's standard flags; leaving them out would also skip the quality checks on the result.
# -lmosquitto links the client library.
do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} ${S}/gateway-publisher.c -o gateway-publisher -lmosquitto
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 gateway-publisher ${D}${bindir}/gateway-publisher

	install -d ${D}${sysconfdir}/init.d
	install -m 0755 ${UNPACKDIR}/gateway-publisher.init ${D}${sysconfdir}/init.d/gateway-publisher
}
