# meta-gateway

My Yocto layer for the gateway image (Raspberry Pi Zero 2 W): the image recipe, my kernel SPI
driver with its device-tree overlay, the MQTT publisher and its init script, and the small
changes to other layers' recipes (Wi-Fi, the network interface file, the Mosquitto broker, the
boot `config.txt`).

Every recipe, with its reason and its ADR, is listed in
[`linux-gateway/README.md`](../README.md#the-layer-meta-gateway). The state of the image and
the tests on the board are in [`docs/architecture.md`](../../docs/architecture.md).

## Dependencies

- Release: Wrynose (`LAYERSERIES_COMPAT_meta-gateway = "wrynose"`).
- `layer.conf` declares only `core` (`LAYERDEPENDS`). The recipes also need `meta-raspberrypi`
  (the machine `raspberrypi0-2w-64` and the `rpi-config` file that I extend) and
  `meta-networking` (Mosquitto, which needs `meta-oe` and `meta-python`). The commits of all the
  repositories are in the table of [`linux-gateway/README.md`](../README.md#the-five-repositories).

## Using it

In this project the layer is listed in `conf/bblayers.conf` as `${TOPDIR}/../meta-gateway`, next
to the other layers, and the image is built with `bitbake gateway-image`. The steps, and the two
files that must exist outside the repo (the Wi-Fi network and the root password hash), are in
[`linux-gateway/README.md`](../README.md).

## License

`COPYING.MIT` is the license text that came with the layer. The kernel module
(`recipes-kernel/stm32-node/files/stm32_node.c`) declares `MODULE_LICENSE("GPL")`.

Maintainer: Marios-Georgios Dimopoulos.
