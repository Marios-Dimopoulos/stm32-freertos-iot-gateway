# linux-gateway

Phase 3 and 4: the custom Linux image of the gateway (Raspberry Pi Zero 2 W), built with
Yocto (ADR-0011). This folder holds only what I wrote myself. The Yocto repositories and
the build directory (63 GB) live **outside** this repo, in `~/yocto/` in WSL2.

## What is here

| Path | What |
|---|---|
| `conf/bblayers.conf` | which layers BitBake reads. An exact copy of the file that built the image. It has absolute paths (`/home/mariosdimopoulos/yocto/...`), so the folders must be at `~/yocto/` for the same user name, or the paths must be edited. |
| `conf/local.conf` | my settings: machine, distribution, serial console, login features and two workarounds. A trimmed copy: the template's comments are dropped and mine say why. Its active lines are identical to the real file. |

My own layer (the Phase 4 driver, the Device Tree overlay, the MQTT recipe) does not exist
yet. It will go here.

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
3. Replace the two generated files in `~/yocto/build/conf/` with `conf/bblayers.conf` and
   `conf/local.conf` from here.
4. Build: `bitbake core-image-base`. The first build took 4 h 12 min on this machine.
5. The image is in `~/yocto/build/tmp/deploy/images/raspberrypi0-2w-64/`, as
   `core-image-base-raspberrypi0-2w-64.rootfs-<date>.wic.bz2`.

The current state of the image is in `docs/architecture.md`.
