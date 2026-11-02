# linux-gateway

Phases 3 and 4: the custom Linux image of the gateway (Raspberry Pi Zero 2 W), built with Yocto
(ADR-0011), with my kernel SPI driver and the MQTT publisher in it. This folder holds only what I
wrote myself. The Yocto repositories and the build folder (about 63 GB) live **outside** this repo,
in `~/yocto/` in WSL2.

The current state of the image, with the tests on the board, is in
[`docs/architecture.md`](../docs/architecture.md).

## What the image is

| | |
|---|---|
| Yocto release | Wrynose 6.0 (LTS), distribution `poky` (Poky 6.0.4) |
| Machine | `raspberrypi0-2w-64` (64-bit) |
| Kernel | Linux 6.18.33 (the Raspberry Pi fork, from `meta-raspberrypi`) |
| Init | sysvinit with BusyBox; no systemd, no package manager, no Python |
| Size | 39 MB root filesystem, 82 packages, 36 MB compressed (`core-image-base`: 142 MB, 2015 packages) |
| Runs at boot | Wi-Fi (`wpa_supplicant`, DHCP), `sshd` (key only), syslog, the Mosquitto broker (`S30`, loopback only), `gateway-publisher` (`S40`) |
| Driver | `stm32_node.ko`, attached to SPI chip select 0 by a device-tree overlay and loaded by udev |

## What is here

| Path | What |
|---|---|
| `conf/bblayers.conf` | The seven layers BitBake reads. The paths are `${TOPDIR}/../<layer>`, so the file works for any user name, but the build folder and the layers must be in the same parent folder (`~/yocto/build` next to `~/yocto/meta-gateway` and the others). Identical to the file that builds the image. |
| `conf/local.conf` | My settings: machine, distribution, serial console, SPI on (`ENABLE_SPI_BUS`), no HDMI login (`USE_VT`), the Wi-Fi firmware license flag (ADR-0013) and the `perl` workaround (ADR-0014). A trimmed copy: the template's comments are dropped and mine say why each line is there. Its active lines are identical to the real file. |
| `meta-gateway/` | My own layer (ADR-0016). Its recipes are listed below. |

### The layer `meta-gateway`

| Recipe or file | What it does | ADR |
|---|---|---|
| `recipes-core/images/gateway-image.bb` | The image recipe: the package list (each name with its reason), the root password hash, the SSH public key with password login off, and the overlay in the boot partition. | 0017, 0015 |
| `recipes-core/images/files/root_authorized_keys` | My SSH **public** key, installed as `/home/root/.ssh/authorized_keys`. Replace it with yours to log in with your key. | 0015 |
| `recipes-core/init-ifupdown/` | My `/etc/network/interfaces`: `auto wlan0`, the `nl80211` driver interface, and Wi-Fi power save off after `ifup`. | 0016 |
| `recipes-connectivity/wpa-supplicant/` | Puts the secrets folder first in the search path, so the image gets my Wi-Fi network (see below). | 0015 |
| `recipes-connectivity/mosquitto/` | The broker without TLS and WebSocket support, and my `mosquitto.conf`: `listener 1883 127.0.0.1`, `allow_anonymous true`. | 0020, 0021 |
| `recipes-kernel/stm32-node/` | My kernel module `stm32_node`: an SPI driver with the character device `/dev/stm32_sensor_node`. Every `read()` does two transfers at least 200 µs apart and returns the second frame if its marker and CRC are right. | 0018, 0019 |
| `recipes-bsp/stm32-node-overlay/` | The device-tree overlay `stm32-node.dtbo`: it disables `spidev@0` and adds `stm32@0` on chip select 0 at 1 MHz. | 0018 |
| `recipes-bsp/bootfiles/rpi-config_%.bbappend` | Adds `dtoverlay=stm32-node` to `config.txt`, and turns off the GPU overlay (it reserved 256 MiB of memory). | 0017, 0018 |
| `recipes-app/gateway-publisher/` | My C program that reads the device and publishes JSON to `gateway/stm32/bmp280`, and its init script (`S40`, `K20`). | 0020 to 0022 |
| `recipes-test/spi-frame-test/` | The bring-up tool of Phase 3 that read frames through `/dev/spidev0.0`. Since the overlay takes chip select 0, it has no device to read on this image. | 0017 |

**BitBake reads `~/yocto/meta-gateway`, not this folder.** The copy in git and the copy in `~/yocto/`
are kept the same by hand. On 2026-11-02 they were identical apart from the two files changed that
day in git (`gateway-image.bb` and `stm32_node.c`), which are not built yet.

## The Wi-Fi key is not in this repo

The image contains my Wi-Fi network, but the key is in a folder outside the repo,
`~/yocto/secrets/` (ADR-0015, update of 2026-10-23). The `.bbappend` in
`meta-gateway/recipes-connectivity/wpa-supplicant/` puts that folder first in the search path of the
recipe, so the file `~/yocto/secrets/wpa_supplicant.conf-sane` is installed as
`/etc/wpa_supplicant.conf`.

That `.bbappend` still names the folder with an absolute path (`/home/mariosdimopoulos/yocto/secrets`).
With another user name, change that line. The root password file below already uses a path relative
to the build folder.

To rebuild with your own network, create the file (mode 600, folder mode 700) with these contents,
with your values in place of the two in angle brackets:

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

If the folder or the file is missing, the build still succeeds and installs the placeholder network
(`key_mgmt=NONE`), so the Pi has no Wi-Fi. Check the result in the root filesystem folder after a
build.

## The root password is not in this repo either

Root has a password, used only on the serial console (SSH accepts a key only). The image has only
its hash, and the hash is in a file outside the repo, `~/yocto/secrets/root.hash` (ADR-0015, update of
2026-10-24). The function `set_root_password` in `gateway-image.bb` finds it at
`${TOPDIR}/../secrets/root.hash`, that is, in `secrets/` next to the build folder, and writes it into
`/etc/shadow` after the root filesystem is built. To make the file, run this in WSL. It asks for the
password twice and does not show it:

```
openssl passwd -6 > ~/yocto/secrets/root.hash
chmod 600 ~/yocto/secrets/root.hash
```

Unlike the Wi-Fi file, this one is checked. The build stops with a message starting with
`set_root_password:` if the file is missing, is empty, or does not start with `$6$`.

**The `.wic.bz2` image and the build folder contain the Wi-Fi key and the password hash: do not
share them.** Do not build with `bitbake -v` either: the verbose log may print the commands.

## The five repositories

Cloned by hand (ADR-0012; `meta-openembedded` added on 2026-10-31, ADR-0020). These are the commits
that build the image (checked in `~/yocto/` on 2026-11-02):

| Folder in `~/yocto/` | Repo | Branch | Commit | Layers used |
|---|---|---|---|---|
| `bitbake` | https://git.openembedded.org/bitbake | `2.18` | `6e260eb6ab16` | (the build tool) |
| `openembedded-core` | https://git.openembedded.org/openembedded-core | `wrynose` | `636e5d3b12d5` | `meta` |
| `meta-yocto` | https://git.yoctoproject.org/meta-yocto | `wrynose` | `7c25b1115655` | `meta-poky` |
| `meta-raspberrypi` | https://github.com/agherzan/meta-raspberrypi | `wrynose` | `919588393f41` | `meta-raspberrypi` |
| `meta-openembedded` | https://git.openembedded.org/meta-openembedded | `wrynose` | `c734c2cb9713` | `meta-oe`, `meta-python`, `meta-networking` |

The seventh layer is `meta-gateway`. Mosquitto is in `meta-networking`, which needs `meta-oe` and
`meta-python`. `meta-python` adds no Python to the image.

## To rebuild the image (not tested from an empty folder)

These steps follow from how the folders are laid out. I have not rebuilt from nothing with them, so
the first time may need a correction.

1. In WSL2 (Ubuntu 24.04), with the Yocto host packages installed: clone the five repos into
   `~/yocto/` and check out the commit of each (table above).
2. Create the build folder with the Yocto script:
   `source ~/yocto/openembedded-core/oe-init-build-env ~/yocto/build`
3. Copy `meta-gateway/` to `~/yocto/meta-gateway`, and replace the two generated files in
   `~/yocto/build/conf/` with `conf/bblayers.conf` and `conf/local.conf` from here.
4. Create `~/yocto/secrets/` (mode 700) with the Wi-Fi file and `root.hash` (see above), and put your
   SSH public key in `meta-gateway/recipes-core/images/files/root_authorized_keys`.
5. Build: `bitbake gateway-image`. The first build of `core-image-base` took 4 h 12 min on this
   machine. Later builds reuse most of it from the cache. The first build of the kernel module
   recompiled the whole kernel (over 14 minutes).
6. The image is in `~/yocto/build/tmp/deploy/images/raspberrypi0-2w-64/`, as
   `gateway-image-raspberrypi0-2w-64.rootfs-<date>.wic.bz2`. I flash it to a microSD card with
   Raspberry Pi Imager ("Use custom", without its customisation settings).

After every build I look for zero-byte files in the root filesystem folder. The first OpenSSH build
once shipped seven empty programs (ADR-0015).

## Checking a flashed board

```sh
ssh -i <your key> root@<pi address>
dmesg | grep stm32_node                    # probe: chip select 0, max clock 1000000 Hz, mode 4
ls -l /dev/stm32_sensor_node               # the driver's device file
mosquitto_sub -t 'gateway/#' -v            # one JSON message per second
grep gateway-publisher /var/log/messages   # the publisher's log (logread does not work here)
```

The Pi has no clock, so the dates in the log are not real; the order of the lines is.
