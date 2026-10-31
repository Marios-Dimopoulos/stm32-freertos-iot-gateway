# The MQTT broker of the gateway (ADR-0020, ADR-0021). The recipe is in meta-networking
# (meta-openembedded); this file changes two things in it without touching that layer.

# 1. Build the broker without TLS and without WebSocket support. The recipe's default is
#    PACKAGECONFIG ??= "ssl websockets ...", which links the broker against OpenSSL and
#    libwebsockets. I need neither: the broker listens on the loopback address only (see
#    mosquitto.conf), so nothing crosses the Wi-Fi, and the Pi has no clock, so TLS certificates
#    could not be checked anyway (ADR-0021). Checked after the build: the broker links only libm,
#    libcjson and libc, and libwebsockets is not in the image. OpenSSL is in the image anyway,
#    because wpa_supplicant and sshd use it, so this saves no OpenSSL package; it keeps TLS code
#    out of the broker and libwebsockets out of the image. This is PACKAGECONFIG of THIS recipe
#    only, the same idea as VC4GRAPHICS in rpi-config_%.bbappend: the smallest scope that does
#    the job.
PACKAGECONFIG:remove = "ssl websockets"

# 2. Install my own mosquitto.conf. The recipe's do_install:append copies the sample file of the
#    source tarball (${S}/mosquitto.conf, all comments) to /etc/mosquitto/. I put the folder next
#    to this file in front of the search path for `file://` entries, add my file to SRC_URI, and
#    copy it over the sample in my own do_install:append. The append of a .bbappend is added
#    after the append of the recipe, so mine runs last and wins.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI += "file://mosquitto.conf"

do_install:append() {
    install -m 0644 ${UNPACKDIR}/mosquitto.conf ${D}${sysconfdir}/mosquitto/mosquitto.conf
}
