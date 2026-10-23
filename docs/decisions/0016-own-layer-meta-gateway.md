# ADR-0016: A layer of my own (`meta-gateway`) that replaces `/etc/network/interfaces`

**Status:** Accepted
**Date:** 2026-10-23
**Phase:** 3

## Context

ADR-0015 left two things open: `wlan0` does not start at boot, and `wpa_supplicant` is started
with the old `wext` interface (kernel warning plus two `ioctl[SIOCSIWENCODEEXT]` errors). Both
come from one file, `/etc/network/interfaces`, which the image gets from the recipe
`init-ifupdown` in `openembedded-core`. Its `wlan0` entry has no `auto` line and says
`wpa-driver wext`.

That recipe installs the file from `init-ifupdown-1.0/interfaces` (`SRC_URI` has
`file://interfaces`, and its `do_install` copies it). Whatever I do has to change that one file
and nothing else, and it has to survive a reflash and a rebuild.

## Options considered

### Option A: a layer of my own with a `.bbappend` and my copy of the file
- How it works: the layer `meta-gateway` is listed in `bblayers.conf`. It holds
  `init-ifupdown_%.bbappend` with one line, `FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"`,
  and my copy of `interfaces` in the folder next to it. While BitBake parses the recipe, the
  `.bbappend` is read with it, so my folder is first in the list of folders where `file://`
  entries are looked for. `do_fetch` finds my file first; the recipe's own `do_install`
  installs it.
- Pros: no file of the other layers is edited; the change is small and visible in my repo; the
  layer is where the next changes (SPI, SSH keys, the Phase 4 driver recipe) belong anyway.
- Cons: a layer is one more thing to keep in step with the build (see Consequences); my copy
  of the file does not follow later changes of the original.

### Option B: edit the file in `openembedded-core`
- Pros: nothing new to create.
- Cons: that folder is a clone of someone else's repo at a fixed commit (ADR-0012). The edit is
  not in my repo, and an update or a `git checkout` loses it or conflicts with it.

### Option C: change the file in the finished root filesystem
- How it works: a `ROOTFS_POSTPROCESS_COMMAND` in `local.conf` that runs `sed` on
  `etc/network/interfaces` after the packages are installed.
- Pros: no layer needed.
- Cons: the recipe still installs the original, and a command patches its output: harder to
  read and to debug, and it breaks silently if the original file changes its text.

## Decision

**Option A.** It is the way Yocto expects a recipe to be changed, it touches only the file I
name, and the layer will be needed in Phase 4 anyway.

The two changes in my copy of `interfaces`: `auto wlan0` is added, and `wpa-driver wext` becomes
`wpa-driver nl80211`. Nothing else in the file is changed.

## Consequences

- Result (2026-10-23): `bitbake-layers show-appends` listed my `.bbappend` under
  `init-ifupdown_1.0.bb`. After `bitbake core-image-base` the root filesystem folder had
  `auto wlan0` and `wpa-driver nl80211` in `etc/network/interfaces`, no zero-byte file outside
  `/etc`, `/var` and `/usr/share`, and the image
  `core-image-base-raspberrypi0-2w-64.rootfs-20261022194428.wic.bz2` (78 MiB).
- On the board, after a flash, only `passwd` and `/etc/wpa_supplicant.conf` (`country=GR`, then
  `wpa_passphrase` with the password typed at its prompt, then the plain-text comment line
  deleted) were done by hand. After a reboot, with no `ifup wlan0`, SSH from Windows worked.
  The `wpa_supplicant` process command line (read from `/proc/<pid>/cmdline`) ended with
  `-D nl80211`, and `dmesg` had no line about wireless extensions.
- When this ADR was written, the Wi-Fi password still had to be typed after every flash (the
  start at boot was automatic, the secret was not in the image). Since 2026-10-23 a second
  `.bbappend` in the layer, `wpa-supplicant_%.bbappend`, installs the network from a file
  outside the repo: see "Update 2026-10-23" in ADR-0015.
- The layer exists in two places: `~/yocto/meta-gateway` (the one BitBake reads, because
  `bblayers.conf` lists that path) and `linux-gateway/meta-gateway/` (the copy in git). I
  compared them on 2026-10-23 and they were identical; if I edit one, I must copy to the other.
- `linux-gateway/conf/bblayers.conf` is again an exact copy of the real file and now lists four
  layers.
- Not verified: whether the two `ioctl[SIOCSIWENCODEEXT]` errors are gone (I looked at `dmesg`
  and at the process, not at the output of `wpa_supplicant`); whether the file still behaves as
  wanted if the original recipe's file changes in a new release (my copy would not follow it).
- Follow-up: make the copy in git the one BitBake reads (a link or a path in `bblayers.conf`)
  so there is one copy; SPI; SSH keys and no empty root password.

## Interview angle

"The image did not bring up Wi-Fi at boot because one file from an upstream recipe was missing
a line. I did not edit the upstream repo and I did not patch the output. I made a layer with a
`.bbappend` that puts my folder first in the search path of that recipe, so its own install
step installs my version of the file. I checked it three times: that BitBake lists the
`.bbappend` under the recipe, the file in the root filesystem folder, and the behaviour on the
board after a reboot."
