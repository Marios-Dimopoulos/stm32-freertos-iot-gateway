# The recipe installs /etc/wpa_supplicant.conf from a file called wpa_supplicant.conf-sane.
# I put a folder OUTSIDE this repo first in the search path for `file://` entries, so the
# recipe finds MY wpa_supplicant.conf-sane there (country, SSID and the derived psk) before
# the placeholder one. The secret is never in git; only this path is.
# If the folder or the file is missing, BitBake does not fail: it silently installs the
# placeholder. After every build I check the result in the root filesystem folder.
#
# Why ${TOPDIR}/../secrets and not /home/<me>/yocto/secrets:
# TOPDIR is the build folder (~/yocto/build). BitBake sets it before it reads any layer, which
# is why bblayers.conf can already use ${TOPDIR}/../<layer>. So this path works for any user
# name, as long as `secrets/` sits next to the build folder, like the layers do. The root
# password file in gateway-image.bb is found the same way.
# `:=` expands the path once, when this file is read, so FILESEXTRAPATHS holds the final folder.
# Because a wrong path does not fail the build (see above), the check after the build matters
# more here than for root.hash, which stops the build when its file is missing.
FILESEXTRAPATHS:prepend := "${TOPDIR}/../secrets:"
