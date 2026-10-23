# The recipe installs /etc/wpa_supplicant.conf from a file called wpa_supplicant.conf-sane.
# I put a folder OUTSIDE this repo first in the search path for `file://` entries, so the
# recipe finds MY wpa_supplicant.conf-sane there (country, SSID and the derived psk) before
# the placeholder one. The secret is never in git; only this path is.
# If the folder or the file is missing, BitBake does not fail: it silently installs the
# placeholder. After every build I check the result in the root filesystem folder.
FILESEXTRAPATHS:prepend := "/home/mariosdimopoulos/yocto/secrets:"
