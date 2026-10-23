# I put the folder next to this file in front of the search path for `file://` entries, so
# the recipe finds MY `interfaces` before the one in openembedded-core and installs it.
# Why: the original has no `auto wlan0`, so Wi-Fi did not start at boot. See ADR-0015.
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
