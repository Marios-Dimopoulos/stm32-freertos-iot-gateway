# ADR-0011: Build the gateway image with Yocto

**Status:** Accepted
**Date:** 2026-10-18
**Phase:** 3

## Context

The gateway is a Raspberry Pi Zero 2 W (Cortex-A53, 512 MB of RAM). Until now it runs the
stock Raspberry Pi OS Lite, only as a test jig for the SPI link (ADR-0010). CLAUDE.md
forbids the stock OS for the final deliverable: the gateway has to run a custom-built
Linux image that I can rebuild from source, where I choose what is in it, and where
Phase 4 can add a kernel SPI driver, a Device Tree overlay and an MQTT publisher.

Building such an image by hand (a kernel, a bootloader, a root filesystem, each cross-compiled
for ARM) is not realistic, so a build system is needed: a tool that takes a description of
what I want (text files), downloads and cross-compiles every piece, and produces one image
file to write to the SD card. The two realistic build systems are Yocto and Buildroot.

Facts that bound the choice:

- The builds only run on Linux. I work on Windows, so they run in WSL2 (Ubuntu) on a
  clone of this repo (CLAUDE.md, "Environments"). The machine has 16 GB of RAM, 8 logical
  CPUs and 260 GB free on `C:` (checked 2026-10-18).
- No hardware purchase is needed for the build itself. A microSD card is needed to boot
  the image; if one has to be bought, its cost is flagged against the 50 EUR budget first.
- I know Linux only at the surface (terminal, ssh). Phase 3 is new to me, so the choice
  also has to be one I can learn and then defend.
- The original plan (`docs/plan.md`) recommended Yocto.

## Options considered

### Option A: Yocto Project (Poky, with the `meta-raspberrypi` layer)
- How it works: the build tool (BitBake) reads *recipes*, one per piece of software, grouped
  in *layers* (folders): the base layer, a layer with the Raspberry Pi board support, and a
  layer of my own on top. A small configuration says which board and which software to
  include. It builds everything and produces an image.
- Pros: the standard in embedded Linux job offers, so it is the tool I most need to be able
  to talk about. The layer idea keeps my work (the Phase 4 driver, the MQTT recipe, the
  configuration) separate from everyone else's code, and a board-support layer for the Pi
  already exists. Everything that defines the OS is a few text files in the repo. Builds
  are cached, so later builds are faster than the first.
- Cons: more concepts to learn before the first image (recipes, layers, the configuration
  files). The first build is long and needs a lot of disk and RAM. Error messages are long.
  The learning curve is the steepest of the two.

### Option B: Buildroot
- How it works: one configuration (a menu-driven or text file) says which board and which
  packages. One command builds the toolchain, the kernel, the packages and the root
  filesystem into an image.
- Pros: much simpler to understand and to start. A smaller first build. A small image with
  little configuration.
- Cons: it is used less in the job offers I am preparing for. It has no layer concept, so
  my own additions live in the same tree as the rest, which is harder to keep separate and
  to maintain over several releases.

*(Not considered: stripping Raspberry Pi OS Lite down. It would still be someone else's
prebuilt image that I cannot rebuild from source, which is what CLAUDE.md rules out.)*

## Decision

**Option A, Yocto.** I chose it for two reasons: it is the build system the roles I am
preparing for ask about, and its layer structure fits how this project grows (my driver,
my MQTT recipe and my Device Tree change sit in a layer of my own, separate from the Pi
support). I accept the longer learning curve and the longer builds on purpose: working
through its errors is part of what I want to learn. Buildroot was a legitimate choice and
would reach a booting Pi sooner; the speed was not the goal.

## Consequences

- Makes easier: a reproducible image from text files in the repo; a clean place (my own
  layer) for the Phase 4 work; a direct answer to "have you used Yocto?".
- Makes harder: the first image takes real time and disk. I need to learn recipes, layers
  and the configuration files before I understand what a build does. Errors will be long.
- Forecloses: nothing permanent. The firmware and the SPI protocol do not depend on this
  choice; only the Pi side does.
- The build directory is huge and must stay out of git (`.gitignore`), as CLAUDE.md says.
  Only the layers and configuration files go in the repo.
- The Yocto system-requirements page (release 6.0.3 "Wrynose", read 2026-10-18) lists
  Ubuntu 24.04 as supported, but says that **WSL 2 is not validated** as a build host, and
  gives 32 GB of RAM and 140 GB of disk as the minimum for building its large graphical
  example image on 4 cores. My image will be far smaller, and I raised WSL's memory from
  7.6 GB to 12 GB (`.wslconfig`), but running on WSL2 with 16 GB of RAM is a known risk:
  a slower build, or a build killed for lack of memory, would be recorded here.
- Not verified: the build time and disk figures I have read ("hours", "tens of GB") are
  from memory; I will record the real numbers after the first build.
- Follow-up: the first steps are the environment in WSL2, then a first minimal image that
  only has to boot (serial console, then Wi-Fi and ssh), with the choices of Yocto release,
  the `meta-raspberrypi` branch and the bootloader recorded in further ADRs as they come up.

## Interview angle

"I used Yocto because it is the industry standard for custom embedded Linux, and its layer
model let me keep my own driver, recipes and Device Tree changes separate from the Raspberry
Pi board support, so the whole image is reproducible from a few files in the repo. I
considered Buildroot, which is simpler and faster to a first boot, and I chose Yocto
knowing the longer learning curve was the cost."
