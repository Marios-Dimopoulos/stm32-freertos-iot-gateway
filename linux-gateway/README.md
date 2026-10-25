# linux-gateway

Phase 3 and 4: the custom Linux image of the gateway (Raspberry Pi Zero 2 W), built with
Yocto (ADR-0011). This folder holds only what I wrote myself. The Yocto repositories and
the build directory (63 GB) live **outside** this repo, in `~/yocto/` in WSL2.

## What is here

| Path | What |
|---|---|
| `conf/bblayers.conf` | which layers BitBake reads (four, with `meta-gateway`). An exact copy of the file that built the image. It has absolute paths (`/home/mariosdimopoulos/yocto/...`), so the folders must be at `~/yocto/` for the same user name, or the paths must be edited. |
| `conf/local.conf` | my settings: machine, distribution, serial console, login features, SSH, SPI and two workarounds. A trimmed copy: the template's comments are dropped and mine say why. Its active lines are identical to the real file. |
| `meta-gateway/` | my own layer (ADR-0016). Two `.bbappend` files: one for `init-ifupdown` with my copy of `/etc/network/interfaces` (`auto wlan0`, `nl80211`), one for `wpa-supplicant` that points at the secrets folder (below). One recipe of my own, `spi-frame-test` (a C program that reads and checks the STM32 frames through `/dev/spidev0.0`), the kernel module `stm32-node` (Phase 4, so far only a skeleton that prints a line when it is loaded and unloaded), and the image recipe `gateway-image` (ADR-0017): the list of packages of the image, with the reason for each name in its comments. |

`bblayers.conf` points at `~/yocto/meta-gateway`, so BitBake does not read this folder: the
copy in git and the copy in `~/yocto/` have to be kept the same by hand (they were identical on
2026-10-23). The Phase 4 driver, the Device Tree overlay and the MQTT recipe will go into this
layer too.

## The Wi-Fi key is not in this repo

The image contains my Wi-Fi network, but the key is in a folder outside the repo,
`~/yocto/secrets/` (ADR-0015, update of 2026-10-23). `meta-gateway/recipes-connectivity/wpa-supplicant/wpa-supplicant_%.bbappend`
puts that folder first in the search path of the recipe, so the file
`~/yocto/secrets/wpa_supplicant.conf-sane` is installed as `/etc/wpa_supplicant.conf`.

To rebuild with your own network, create that file (mode 600, folder mode 700) with these
contents, with your values in place of the two in angle brackets:

```
ctrl_interface=/var/run/wpa_supplicant
ctrl_interface_group=0
update_config=1
country=<your country code>

network={
        ssid="<your SSID>"
        psk=<64 hex characters, from wpa_passphrase or PBKDF2-HMAC-SHA1 of the password with the SSID as salt, 4096 iterations>
}
```

If the folder or the file is missing, the build still succeeds and installs the placeholder
network (`key_mgmt=NONE`), so the Pi has no Wi-Fi. The `.wic.bz2` file and the build folder
contain the key: do not share them.

## The root password is not in this repo either

The image has a password for `root`, but only its hash, and the hash is in a file outside the repo,
`~/yocto/secrets/root.hash` (ADR-0015, update of 2026-10-24). The function `set_root_password` in
`meta-gateway/recipes-core/images/gateway-image.bb` writes it into `/etc/shadow` after the root
filesystem is built. To make the file, run this in WSL; it asks for the password twice and does not
show it:

```
openssl passwd -6 > ~/yocto/secrets/root.hash
chmod 600 ~/yocto/secrets/root.hash
```

Unlike the Wi-Fi file, this one is checked: the build stops with a message starting with
`set_root_password:` if the file is missing, empty, or does not start with `$6$`. The `.wic.bz2` file
and the build folder contain the hash: do not share them.

## The four repositories

Cloned by hand on 2026-10-18 (ADR-0012). The commits are the ones that built the image:

| Folder in `~/yocto/` | Repo | Branch | Commit |
|---|---|---|---|
| `bitbake` | https://git.openembedded.org/bitbake | `2.18` | `6e260eb6ab16` |
| `openembedded-core` | https://git.openembedded.org/openembedded-core | `wrynose` | `636e5d3b12d5` |
| `meta-yocto` | https://git.yoctoproject.org/meta-yocto | `wrynose` | `7c25b1115655` |
| `meta-raspberrypi` | https://github.com/agherzan/meta-raspberrypi | `wrynose` | `919588393f41` |

## To rebuild the image (not tested from an empty folder)

These steps follow from how the folders are laid out. I have not rebuilt from nothing with
them, so the first time may need a correction.

1. In WSL2 (Ubuntu 24.04), with the Yocto host packages installed: clone the four repos
   into `~/yocto/` and check out the commit of each.
2. Create the build folder with the Yocto script:
   `source ~/yocto/openembedded-core/oe-init-build-env ~/yocto/build`
3. Copy `meta-gateway/` to `~/yocto/meta-gateway`, create `~/yocto/secrets/` with the two files
   (see above: the Wi-Fi file and `root.hash`), and
   replace the two generated files in `~/yocto/build/conf/` with `conf/bblayers.conf` and
   `conf/local.conf` from here.
4. Build: `bitbake gateway-image`. (The first build, of the standard `core-image-base`, took
   4 h 12 min on this machine; `gateway-image` reuses most of it from the cache.)
5. The image is in `~/yocto/build/tmp/deploy/images/raspberrypi0-2w-64/`, as
   `gateway-image-raspberrypi0-2w-64.rootfs-<date>.wic.bz2`.

The current state of the image is in `docs/architecture.md`.
