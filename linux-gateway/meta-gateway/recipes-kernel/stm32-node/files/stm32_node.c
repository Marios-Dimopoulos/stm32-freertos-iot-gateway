// SPDX-License-Identifier: GPL-2.0-only
/*
 * stm32_node: driver for the STM32 sensor node on the Raspberry Pi's SPI bus.
 *
 * Step 4.3 made the module an SPI driver: it attaches to a device when the kernel finds one that
 * matches (probe) and detaches when the device or the driver goes away (remove). The device comes
 * from the device-tree overlay, which adds a node with the compatible string below on chip select 0.
 *
 * Step 4.4 (this file): probe does ONE SPI transfer of 14 bytes, checks the marker and the CRC of
 * the frame, and prints the result in the kernel log. That proves that the kernel can talk to the
 * node. The character device, the minimum gap between transfers and the policy for the stale first
 * read come in step 4.5; until then nothing else uses the transfer.
 *
 * The frame layout and the CRC are in firmware/stm32-sensor-node/Src/frame.h and
 * docs/decisions/0010-spi-link-design.md. The same checks are done in user space by
 * spi-frame-test.c (recipes-test), which is the code this file was written from.
 */
#include <linux/module.h>
#include <linux/spi/spi.h>

/* Puts the module name in front of every pr_*() line. dev_info() below prints the device name
 * instead ("stm32_node spi0.0: ..."), so this only affects pr_*(). */
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define FRAME_SIZE	14
#define FRAME_MARKER	0xA5
#define CRC_BYTES	12	/* the CRC covers bytes 0..11; it is stored in bytes 12..13 */
#define CRC_START	0xFFFF
#define CRC_POLYNOMIAL	0x1021

/* Offsets in the frame. Numbers are little-endian, as the STM32 wrote them. */
#define OFFSET_MARKER	0
#define OFFSET_STATUS	1
#define OFFSET_SEQUENCE	2	/* 2 bytes */
#define OFFSET_CRC	12	/* 2 bytes */
/* Bytes 4 to 11 (temperature and pressure) are not decoded here: the program that reads the frame
 * does it (docs/decisions: the driver checks the frame, user space decides what it means). */

/*
 * CRC-16/CCITT-FALSE: polynomial 0x1021, start 0xFFFF, no reflection, no final XOR. It is the same
 * function as in the firmware and in spi-frame-test.c; a different variant would reject every
 * frame. I wrote it here instead of calling the kernel's crc_itu_t() because I have not checked
 * that crc_itu_t() gives this exact variant, nor that this kernel was built with it.
 */
static u16 crc16(const u8 *data, int length)
{
	u16 crc = CRC_START;
	int i, bit;

	for (i = 0; i < length; i++) {
		crc ^= (u16)data[i] << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000) ? (u16)((crc << 1) ^ CRC_POLYNOMIAL) : (u16)(crc << 1);
	}
	return crc;
}

/*
 * Reads one frame from the node. Returns 0 if the SPI transfer itself worked (the bytes are then
 * in rx, good or not) and a negative error number if the transfer failed.
 *
 * The Pi is the SPI master, so the node only sends when the Pi clocks it. A single transfer of 14
 * bytes keeps NSS low for the whole time: the node's DMA sends the whole frame in that one
 * transaction. Two smaller transfers would be two transactions, and the second would not continue
 * the frame (the same rule as in spi-frame-test.c).
 *
 * speed_hz is left at 0, which makes the SPI core use spi->max_speed_hz, the 1 MHz that the
 * device-tree node asked for (well under the 6 MHz limit of the project).
 */
static int stm32_node_read_frame(struct spi_device *spi, const u8 *tx, u8 *rx)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = FRAME_SIZE,
	};

	/* Blocking call, on purpose and for now: 14 bytes at 1 MHz take about 112 us, so this is
	 * not a bulk transfer, and probe may sleep. The SPI core and the controller driver choose
	 * by themselves whether to use DMA for a transfer. */
	return spi_sync_transfer(spi, &xfer, 1);
}

/*
 * Called by the kernel when it finds an SPI device that matches this driver: that is, when a
 * device-tree node with a compatible string from the table below exists on the bus. `spi`
 * describes that device: which chip select it is on, the maximum clock the node asked for, and
 * the SPI mode. The kernel has already read these from the device-tree node.
 *
 * A return value of 0 means "I accept this device". A negative error number means "no", and the
 * kernel then leaves the device without a driver.
 *
 * If the node does not answer with a good frame (the STM32 is off, a wire is loose, the CRC is
 * wrong), probe still returns 0 and prints a warning. The device exists on the bus either way, and
 * the node may be powered later; the reads of step 4.5 will try again. Returning an error here
 * would make one bad frame at boot remove the driver for good. Only a failed memory allocation
 * makes probe fail.
 */
static int stm32_node_probe(struct spi_device *spi)
{
	u8 *tx, *rx;
	int ret;

	/* The mode is printed as the SPI core set it. The value 4 is not a clock mode: bits 0 and 1
	 * (CPHA, CPOL) are 0, so the clock is mode 0, which the node expects. Bit 2 is SPI_CS_HIGH,
	 * set by the SPI core itself (drivers/spi/spi.c, "forcing CS_HIGH") when the controller
	 * drives the chip select through a GPIO descriptor, because the descriptor already holds
	 * the active-low polarity from the device tree. The line is still active low on the wire. */
	dev_info(&spi->dev, "probe: chip select %u, max clock %u Hz, mode %u\n",
		 spi_get_chipselect(spi, 0), spi->max_speed_hz, spi->mode);

	/* The buffers come from devm_kzalloc(), not from the stack: a buffer that a controller may
	 * use with DMA has to be DMA-safe memory, and kernel stacks are not (they can be mapped in
	 * a way that has no physical address). Separate allocations keep tx and rx on different
	 * cache lines. devm_ means the kernel frees them when the driver is detached. The tx buffer
	 * is all zeros, because the node ignores what the master sends. */
	tx = devm_kzalloc(&spi->dev, FRAME_SIZE, GFP_KERNEL);
	rx = devm_kzalloc(&spi->dev, FRAME_SIZE, GFP_KERNEL);
	if (!tx || !rx)
		return -ENOMEM;

	ret = stm32_node_read_frame(spi, tx, rx);
	if (ret) {
		dev_warn(&spi->dev, "SPI transfer failed: %d\n", ret);
		return 0;
	}

	/* %*ph prints a buffer as hex bytes ("A5 00 1A ..."). */
	dev_info(&spi->dev, "frame: %*ph\n", FRAME_SIZE, rx);

	if (rx[OFFSET_MARKER] != FRAME_MARKER) {
		/* A dead or missing node reads as all 00 or all FF, never 0xA5 (frame.h). */
		dev_warn(&spi->dev, "bad marker 0x%02X (expected 0x%02X): node off or not connected?\n",
			 rx[OFFSET_MARKER], FRAME_MARKER);
		return 0;
	}
	/* The node stored its CRC in two bytes, low byte first (little-endian), so the 16-bit
	 * number is the first byte plus the second one shifted left by 8 bits. */
	if (crc16(rx, CRC_BYTES) != (rx[OFFSET_CRC] | (rx[OFFSET_CRC + 1] << 8))) {
		dev_warn(&spi->dev, "bad CRC: the frame was damaged on the wire\n");
		return 0;
	}

	dev_info(&spi->dev, "good frame: seq=%u status=%u\n",
		 rx[OFFSET_SEQUENCE] | (rx[OFFSET_SEQUENCE + 1] << 8), rx[OFFSET_STATUS]);
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
