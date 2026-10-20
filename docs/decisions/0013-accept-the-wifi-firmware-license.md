# ADR-0013: Accept the `synaptics-killswitch` license flag to get the Wi-Fi firmware

**Status:** Accepted
**Date:** 2026-10-18
**Phase:** 3

## Context

The first `bitbake core-image-base` for the machine `raspberrypi0-2w-64` stopped within
seconds, before any download or compilation, with:

```
ERROR: Nothing RPROVIDES 'linux-firmware-rpidistro-bcm43436' ...
linux-firmware-rpidistro ... was skipped: Has a restricted license 'synaptics-killswitch'
which is not listed in your LICENSE_FLAGS_ACCEPTED.
```

The Pi Zero 2 W has a Wi-Fi/Bluetooth chip whose driver needs firmware files from the chip
vendor. The machine file of `meta-raspberrypi` (`conf/machine/raspberrypi0-2w-64.conf`)
recommends them, and the recipe `linux-firmware-rpidistro_git.bb` marks them with
`LICENSE_FLAGS = "synaptics-killswitch"`. The layer's own document (`docs/ipcompliance.md`)
says that some of these firmware blobs are under the Synaptics license, one clause of which
"can be (at least theoretically) used as a killswitch", that this was reported upstream,
and that the build therefore refuses the recipe unless the user acknowledges the risk
with `LICENSE_FLAGS_ACCEPTED = "synaptics-killswitch"`.

The gateway has no Ethernet port: Wi-Fi is the only network the Zero 2 W has, and Phase 4
(MQTT) needs it. The layer's reference configuration file (`kas-poky-rpi.yml`) already
contained this line, commented out, with a pointer to the same document. I read it earlier
and did not notice what it was for.

## Options considered

### Option A: accept the license flag
- How it works: add `LICENSE_FLAGS_ACCEPTED = "synaptics-killswitch"` to `local.conf`.
  The firmware is built into the image.
- Pros: Wi-Fi and Bluetooth work, which Phase 4 needs. It is the same firmware the stock
  Raspberry Pi OS ships. The acknowledgement is explicit and in the repo, not hidden.
- Cons: I accept a license clause that could in theory be used to disable the firmware.
  For a personal learning project the practical risk is low; a product would need a
  review of the license first.

### Option B: leave the Wi-Fi firmware out
- How it works: remove it from `MACHINE_EXTRA_RRECOMMENDS`, so nothing needs the flag.
- Pros: nothing to accept.
- Cons: no Wi-Fi in the image, so Phase 4 could not publish anything; I would have to come
  back to Option A later and rebuild.

## Decision

**Option A.** The gateway cannot work without Wi-Fi, so leaving the firmware out would only
postpone the decision. I accept the flag knowingly: the line in `local.conf` carries a
comment that says why.

## Consequences

- Makes easier: the image builds; Wi-Fi can be brought up in a later step.
- Makes harder: the image contains firmware under a restricted license, so redistributing
  the image (or a product built from it) would need a license review. This repo contains
  only the configuration, not the image or the firmware.
- The same `local.conf` currently also contains an empty root password for development
  (`empty-root-password allow-empty-password allow-root-login`). That is insecure by design
  and must be removed before any final image; it gets its own entry when the image is
  hardened.
- Not verified: that the build then succeeds and that the firmware loads on the board (the
  Wi-Fi bring-up is a later step).

## Interview angle

"Yocto refused to build the image because the Wi-Fi firmware has a restricted license. I
read the layer's compliance note, saw the risk was a theoretical kill-switch clause, and
accepted it explicitly in the configuration with a comment, because the gateway needs
Wi-Fi. For a real product I would have had the license reviewed first."
