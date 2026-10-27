// SPDX-License-Identifier: GPL-2.0-only
/*
 * stm32_node: driver for the STM32 sensor node on the Raspberry Pi's SPI bus.
 *
 * Step 4.3 of Phase 4: the module becomes an SPI driver. It still does no SPI transfer. All it
 * does is attach to a device when the kernel finds one that matches (probe), and detach when the
 * device or the driver goes away (remove), and print a line in each case. The device comes from
 * the device-tree overlay, which adds a node with the compatible string below on chip select 0.
 * The transfer, the frame checks and the character device come in the next steps.
 */
#include <linux/module.h>
#include <linux/spi/spi.h>

/* Puts the module name in front of every pr_*() line. dev_info() below prints the device name
 * instead ("stm32_node spi0.0: ..."), so this only affects pr_*(). */
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/*
 * Called by the kernel when it finds an SPI device that matches this driver: that is, when a
 * device-tree node with a compatible string from the table below exists on the bus. `spi`
 * describes that device: which chip select it is on, the maximum clock the node asked for, and
 * the SPI mode. The kernel has already read these from the device-tree node.
 *
 * A return value of 0 means "I accept this device". A negative error number means "no", and the
 * kernel then leaves the device without a driver.
 */
static int stm32_node_probe(struct spi_device *spi)
{
	dev_info(&spi->dev, "probe: chip select %u, max clock %u Hz, mode %u\n",
		 spi_get_chipselect(spi, 0), spi->max_speed_hz, spi->mode);
	return 0;
}

/* Called when the device goes away or the driver is unloaded: undo what probe did. */
static void stm32_node_remove(struct spi_device *spi)
{
	dev_info(&spi->dev, "remove\n");
}

/* The names this driver handles, as written in the device tree: "<vendor>,<device>". The kernel
 * compares them with the compatible string of every SPI node. The empty entry ends the table. */
static const struct of_device_id stm32_node_of_match[] = {
	{ .compatible = "mdimopoulos,stm32-sensor-node" },
	{ }
};
MODULE_DEVICE_TABLE(of, stm32_node_of_match);

/* The same device by its SPI name: the part of the compatible string after the comma. The SPI
 * core builds the module alias of a device from this name ("spi:stm32-sensor-node"), which is
 * what lets udev load this module by itself when the node appears. Without this table the
 * kernel also prints a warning for every device that is matched by the device tree only. Both
 * statements are from my memory of the SPI core, not tested here: the boot will show if udev
 * loads the module without a modprobe. */
static const struct spi_device_id stm32_node_spi_ids[] = {
	{ "stm32-sensor-node" },
	{ }
};
MODULE_DEVICE_TABLE(spi, stm32_node_spi_ids);

static struct spi_driver stm32_node_driver = {
	.driver = {
		.name = "stm32_node",
		.of_match_table = stm32_node_of_match,
	},
	.id_table = stm32_node_spi_ids,
	.probe = stm32_node_probe,
	.remove = stm32_node_remove,
};

/* Generates module_init() and module_exit() for us: the first registers the driver with the SPI
 * core (spi_register_driver), the second removes it (spi_unregister_driver). This replaces the
 * two functions of step 4.2 that printed "loaded" and "unloaded". */
module_spi_driver(stm32_node_driver);

/* The license is not decoration: a module that says it is not GPL "taints" the kernel and
 * cannot use the kernel functions that are exported to GPL modules only. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("STM32 sensor node on the SPI bus");
MODULE_AUTHOR("Marios-Georgios Dimopoulos");
