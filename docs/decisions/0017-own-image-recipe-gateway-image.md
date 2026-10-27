# ADR-0017: An image recipe of my own (`gateway-image`), built up from the minimum

**Status:** Accepted
**Date:** 2026-10-24
**Phase:** 3

## Context

The image I had been using, `core-image-base`, worked, but it was large and full of things a
headless gateway does not use. Measured on 2026-10-24 (root filesystem folder, package manifest,
compressed image):

- 142 MB root filesystem, 2015 packages, 78 MiB compressed;
- audio, Bluetooth, X11 and graphics libraries (`libasound`, `libX11`, `bluetoothctl`, `btmon`, ...);
- every kernel module (28 MB), because `meta-raspberrypi` sets
  `MACHINE_EXTRA_RRECOMMENDS += "kernel-modules udev-rules-rpi"`;
- 17 MB in `/etc/udev` and the services `bluetooth`, `ofono`, `neard`, `avahi-daemon`, `alsa-state`.

The reason is where the package list comes from. `core-image-base` inherits `core-image`,
whose list is `packagegroup-core-boot` plus `packagegroup-base-extended`; the second one adds
packages according to `MACHINE_FEATURES` (the machine file of the Pi sets `alsa bluetooth wifi
sdio usbhost keyboard screen touchscreen vc4graphics`). The image recipe cannot change
`MACHINE_FEATURES`, so removing things from outside means pulling them out one by one.

## Options considered

### Option A: subtract, in `local.conf`
- How it works: keep `core-image-base` and remove: `DISTRO_FEATURES:remove`,
  `IMAGE_FEATURES:remove`, `BAD_RECOMMENDATIONS`, `PACKAGE_EXCLUDE`.
- Pros: small steps; each removal can be tested alone.
- Cons: I fight the defaults of a packagegroup tree I did not write. `IMAGE_INSTALL:remove` only
  removes names listed directly, and most unwanted packages arrive through `RDEPENDS` of a
  packagegroup. I never see the final list in one place.

### Option B: an image recipe of my own that starts from the minimum and adds
- How it works: `gateway-image.bb` in `meta-gateway`, `inherit core-image` (for the image tasks
  and the `IMAGE_FEATURES` table) with `IMAGE_INSTALL = "packagegroup-core-boot ..."` instead of
  the class default. I add what the gateway needs, one step at a time, with a build, a flash and
  a test after each.
- Pros: every package is in the image because I named it. The list is in one file in my layer.
  A failure after a step has one cause.
- Cons: the first versions may not boot or may miss a driver; I learn that by testing.

### Option C: start from `core-image-minimal`
- Pros: a tested, smaller starting point.
- Cons: it still contains what that recipe contains, and I do not control it.

## Decision

**Option B.** It matches the project's rule that the final image is custom, and it makes the
package list something I can read and defend.

## Consequences

The steps, each built, flashed and tested on the board:

| Step | List | Root fs | Packages | Compressed | Result |
|---|---|---|---|---|---|
| 1 | `packagegroup-core-boot` | 29 MB | 47 | 32 MB | booted to a login on the serial console; no `wlan0`, no `/dev/spi*` |
| 2 | + Wi-Fi: `wpa-supplicant`, `iw`, `wireless-regdb-static`, firmware, `brcmfmac` modules | 65 MB | 79 | 45 MB | first test failed (below), then `wlan0` up with an address |
| 3 | + `kernel-module-spi-bcm2835`, `kernel-module-spidev`, `spi-frame-test` | | | 46 MB | `/dev/spidev0.0`; 10 of 10 frames with a valid CRC |
| 4 | no kernel copy, no language files | **37 MB** | **76** | **35 MB** | boots from the kernel on partition 1; Wi-Fi and SPI as before |

Against `core-image-base`: 74% smaller root filesystem, 96% fewer packages, 55% smaller image.

What went wrong on the way, with the evidence:

- **No `wlan0` after step 2.** `lsmod` showed `brcmfmac`, so the driver loaded; `dmesg` said
  `Direct firmware load for brcm/brcmfmac43430b0-sdio.raspberrypi,model-zero-2-w.bin failed with
  error -2`. The chip reports itself as BCM43430/2, so the driver asks for a `43430b0` name; that
  name is a symlink to the 43436 firmware and lives in the package
  `linux-firmware-rpidistro-bcm43430`, which I had not listed (the real file is in `...bcm43436`).
  I had guessed that `...bcm43436s` was missing; that was wrong. The old image had both packages.
- **Only one of three `brcmfmac` vendor modules is needed.** I listed all three; `lsmod` on the
  board showed only `brcmfmac_cyw`, so `-wcc` and `-bca` were removed from the list.
- **The 26 MB kernel copy in `/boot` of the root partition.** Every module package depends on
  `kernel-base`, which recommends `kernel-image`. The firmware loads the kernel from partition 1
  (`kernel8.img`) and partition 1 is then mounted over `/boot`, so the copy is never used.
  `BAD_RECOMMENDATIONS += "kernel-image"` did nothing and gave no error: Yocto renames kernel
  packages to include the version (`kernel-image-6.18.33-v8`), and the exclusion matches final
  names. The pattern `kernel-image-*` worked (the entries go to `dnf -x`, which takes patterns).
  The empty folder `/boot` stays on purpose: it is the mount point for partition 1.
- **Language packages.** `BAD_RECOMMENDATIONS` failed the build for `locale-base-*`: they were
  arguments of the `dnf install` command, so they were requests and not recommendations. They come
  from `IMAGE_LINGUAS`, which `image.bbclass` leaves empty, so something in my configuration sets
  it; I did not find what. `IMAGE_LINGUAS = ""` in the recipe fixed it (about 2 MB).

What is not done:

- `kbd` and `keymaps` are still in the image: `packagegroup-core-boot` has `keymaps` in its
  `RDEPENDS` because the machine has the `keyboard` feature. A hard dependency cannot be removed
  with `BAD_RECOMMENDATIONS`; I would have to stop using `packagegroup-core-boot` and list its
  members myself.
- A login prompt on `tty1` (the HDMI console) still runs; the three `ip: SIOCGIFFLAGS: No such
  device` lines at boot are still there (the network script tries interfaces this board does not
  have; not confirmed which).
- `IMAGE_FEATURES` (`ssh-server-openssh` and the root-login features) are still in `local.conf`,
  so they apply to every image. They should move into the recipe.
- The empty root password and the Wi-Fi key inside the image are still there (ADR-0015).
- The kernel is the vendor one with a lot built in (graphics, USB, NFS, iSCSI in the boot log);
  its 26 MB image is not reduced.
- Not measured: boot time and memory use of the final image (the step-1 image showed about 37 MB
  used, as `free` printed it, in KiB).

## Update 2026-10-25: the last clean-up, and what I decided not to do

- **The `tty1` login is gone.** `sysvinit-inittab` adds a `getty` on the HDMI console when `USE_VT`
  is `"1"` (its default, line 61 of the recipe; the test is on line 33). `USE_VT = "0"` in
  `local.conf` removed it. Checked: `/etc/inittab` of the root filesystem has only the `ttyS0`
  line, and on the board `ps` shows one `getty`. The size did not change (37 MB, 76 packages,
  image `...rootfs-20261025113045.wic.bz2`, 35 MiB); it removes a running process.
- **`DISABLE_VC4GRAPHICS = "1"` was tried and undone.** The build failed in
  `cairo-1.18.4 do_configure` with `QA Issue: invalid PACKAGECONFIG(s): egl`. The cause is in
  the vendor layer: `meta-raspberrypi/recipes-graphics/cairo/cairo_%.bbappend`, line 3, appends
  ` egl ${PACKAGECONFIG_GLESV2}` when the machine does **not** have the feature `vc4graphics`,
  and `cairo_1.18.4.bb` of `openembedded-core` defines only `xlib`, `xcb` and `trace`. So the
  vendor `.bbappend` is out of date for this core version. I do not know why `cairo` is part of
  the build of a headless image. A fix would be a `cairo_%.bbappend` in my layer with
  `PACKAGECONFIG:remove = "egl glesv2"`; I did not do it, because the gain is the graphics line in
  `config.txt` and possibly the 256 MiB CMA reservation (the source of that reservation is not
  found; "the `vc4` overlay does it" is only a hypothesis), and other recipes might break the same
  way.
- **`kbd` and `keymaps` stay.** Measured with `PKGSIZE` in the package data: 1,691,087 and 392
  bytes (about 1.7 MB, under 5% of the 37 MB). Removing them means `MACHINE_FEATURES:remove =
  "keyboard"` (they come from `packagegroup-core-boot`, line 24), a variable that many recipes
  read, so the rebuild would be large for a small gain.
- Test on the board: one `getty` (`ttyS0`), `wlan0` with an address, `/dev/spidev0.0`; without the
  Nucleo powered, `spi-frame-test` printed `BAD` frames of all zeros (a dead or missing slave reads as
  `00` or `FF`, see `frame.h`), and with it powered the result was reported as good (I did not see
  that output).

## Interview angle

"The default image pulled in many things a headless gateway never uses, and I could not remove
them from outside, because they come in through packagegroups driven by the machine's features. So I
wrote my own image recipe that starts from the minimum that boots and adds what the gateway needs in
four steps, flashing and testing after each. Three things failed on the way: a firmware symlink that
lives in a different package than the firmware file, a package exclusion that did nothing because
Yocto renames kernel packages with the version, and language packages that were requests and not
recommendations. Each was found from the logs of the board or the build, not by guessing. The root
filesystem went from 142 MB to 37 MB and the packages from 2015 to 76."

(Measured with the same method before and after: `du -sh` of the root filesystem folder, the
number of lines of the package manifest, `ls -lh` of the `.wic.bz2`.)

## Update 2026-10-27: the graphics overlay and the CMA pool

The hypothesis above was right. The base device tree of the Zero 2 W has `linux,cma` with `size =
0x4000000` (64 MiB); the overlay `vc4-fkms-v3d.dtbo` has a fragment that sets `size = 0x10000000`
(256 MiB) on the same node (both read with `dtc -I dtb -O dts` on the files in `deploy/`). The kernel
command line had no `cma=`.

`rpi-config` writes `dtoverlay=${VC4DTBO}` only when its own variable `VC4GRAPHICS` is `1`. That
variable is computed from `MACHINE_FEATURES`, but `rpi-config_%.bbappend` in `meta-gateway` sets it to
`"0"`, so only the `config.txt` line disappears. `MACHINE_FEATURES` is not touched, so `cairo` does
not change, and the `cairo` workaround I considered is not needed.

Result on the board: CMA pool 64 MiB (was 256), `Memory: 350660K/458752K available` (was
154540K), `MemAvailable` 354,656 kB; the `probe` line of `stm32_node` and the SPI nodes are as before.
The build ran 5105 tasks, 5097 not rerun; only the boot files changed. Not done: the remaining 64 MiB
(a `cma=` on the command line); nothing uses it, but I did not test whether the Pi firmware
accepts a smaller value.
