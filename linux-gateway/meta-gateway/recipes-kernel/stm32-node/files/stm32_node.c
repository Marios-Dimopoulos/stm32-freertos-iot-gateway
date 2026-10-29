// SPDX-License-Identifier: GPL-2.0-only
/*
 * stm32_node: driver for the STM32 sensor node on the Raspberry Pi's SPI bus.
 *
 * Step 4.3 made the module an SPI driver: it attaches to a device when the kernel finds one that
 * matches (probe) and detaches when the device or the driver goes away (remove). The device comes
 * from the device-tree overlay, which adds a node with the compatible string below on chip select 0.
 *
 * Step 4.4 did ONE SPI transfer of 14 bytes in probe and checked the marker and the CRC of the
 * frame. That proved that the kernel can talk to the node.
 *
 * Step 4.5a: the transfer moved from probe into read(). probe now creates the file
 * /dev/stm32_sensor_node (a "misc device", the simplest kind of character device), and every
 * read() of that file does one SPI transfer, checks the frame and gives the 14 raw bytes to the
 * program, or an error if the frame is bad. The program decodes the temperature and the pressure
 * (ADR-0018).
 *
 * Step 4.5b: before each transfer the driver waits until at least MIN_GAP_US have
 * passed since the end of the previous one, so that a fast reader cannot start a transaction
 * before the node has loaded the next frame.
 *
 * Step 4.5c (this file): every read() does two transfers and keeps the second, because a transfer
 * returns the frame the node loaded after the previous one (the "stale first read"). The first
 * frame is thrown away.
 *
 * The frame layout and the CRC are in firmware/stm32-sensor-node/Src/frame.h and
 * docs/decisions/0010-spi-link-design.md. The same checks are done in user space by
 * spi-frame-test.c (recipes-test), which is the code this file was written from.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

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
#define OFFSET_CRC	12	/* 2 bytes */
/* Bytes 1 to 11 (status, sequence, temperature and pressure) are not decoded here: the program
 * that reads the frame does it (ADR-0018: the driver checks the frame, user space decides what it
 * means). */

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
 * The least time the node needs between the end of one transaction and the start of the next, to
 * load the next frame into its DMA (the "re-arm", ADR-0010). Phase 2 saw gaps of 154 to 228 us
 * between back-to-back reads from Python, and 30000 such reads were good, so 154 us is known to
 * work. 200 us is a margin I chose above that: the re-arm time itself was never measured (only
 * this upper bound), and a C program or this driver can read much faster than Python.
 */
#define MIN_GAP_US	200

/* When the last transfer ended. It starts at 0, so the first read after the module is loaded
 * does not wait. */
static ktime_t last_transfer_end;

/* Two programs can call read() at the same moment. Without a lock both would see the same
 * last_transfer_end and transfer at once, or too close together; with it they take turns, and
 * each one waits for its own gap. */
static DEFINE_MUTEX(frame_lock);

/*
 * Reads one frame from the node. Returns 0 if the SPI transfer itself worked (the bytes are then
 * in rx, good or not) and a negative error number if the transfer failed.
 *
 * Before the transfer it waits until MIN_GAP_US have passed since the end of the previous one.
 * The wait is only paid when a read comes too soon; a program that reads once a second never
 * waits.
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
	s64 since_us;
	int ret;

	mutex_lock(&frame_lock);

	/* ktime_get() is the kernel's monotonic clock; ktime_us_delta(a, b) is a - b in
	 * microseconds. usleep_range(min, max) sleeps between min and max microseconds; the extra 50
	 * lets the kernel group wake-ups, which costs less than an exact sleep. */
	since_us = ktime_us_delta(ktime_get(), last_transfer_end);
	if (since_us < MIN_GAP_US)
		usleep_range(MIN_GAP_US - since_us, MIN_GAP_US - since_us + 50);

	/* Blocking call, on purpose and for now: 14 bytes at 1 MHz take about 112 us, so this is
	 * not a bulk transfer, and read() may sleep. The SPI core and the controller driver choose
	 * by themselves whether to use DMA for a transfer. */
	ret = spi_sync_transfer(spi, &xfer, 1);

	/* Taken after the transfer returns, which is slightly after NSS went high, so the real gap
	 * is never shorter than MIN_GAP_US. */
	last_transfer_end = ktime_get();

	mutex_unlock(&frame_lock);
	return ret;
}

/*
 * The SPI device, saved by probe so that read() can find it. The kernel calls read() with fixed
 * arguments (the open file, the program's buffer, the size, the position), so nothing in them
 * tells read() which SPI device to use, and probe's local variables are gone after it returns.
 * A global pointer is enough because this project has exactly one STM32 node; with two nodes
 * they would overwrite each other, and the usual fix would be a struct that holds the device and
 * the misc device together, found again with container_of().
 */
static struct spi_device *node_spi;

/*
 * Called by the kernel when a program calls read() on /dev/stm32_sensor_node. buf is an address
 * in the PROGRAM's memory (that is what __user says), count is how many bytes it asked for.
 *
 * Returns the number of bytes read (14) or a negative error number, which the program sees as
 * read() returning -1 with errno set:
 *   -ENODEV  the SPI device is gone (the driver was removed while the file was open)
 *   -EINVAL  the program asked for fewer than 14 bytes; only a whole frame is returned
 *   -ENOMEM  no memory for the buffers
 *   -EIO     the node did not answer with a good frame (wrong marker, which is what a dead or
 *            missing node gives, or wrong CRC). The program cannot tell the two apart.
 *   -EFAULT  buf is not a valid address in the program
 *   others   the SPI transfer itself failed
 */
static ssize_t stm32_node_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	u8 *tx, *rx;
	int ret;

	if (!node_spi)
		return -ENODEV;
	if (count < FRAME_SIZE)
		return -EINVAL;

	/* The buffers come from kzalloc(), not from the stack: a buffer that a controller may use
	 * with DMA has to be DMA-safe memory, and kernel stacks are not (they can be mapped in a way
	 * that has no physical address). Separate allocations keep tx and rx on different cache
	 * lines. They are freed at the end of this call. The tx buffer is all zeros, because the
	 * node ignores what the master sends. */
	tx = kzalloc(FRAME_SIZE, GFP_KERNEL);
	rx = kzalloc(FRAME_SIZE, GFP_KERNEL);
	if (!tx || !rx) {
		ret = -ENOMEM;
		goto out;
	}

	/* The policy for the stale first read (ADR-0010, step 4.5c): the node loads the newest frame
	 * into its output buffer right after every transaction, so a transfer returns the frame
	 * that was loaded after the PREVIOUS one, however long ago that was, or the placeholder if
	 * there was none since the node started. The first transfer is therefore thrown away; what
	 * matters is its side effect, that the node reloads its buffer when it ends. */
	ret = stm32_node_read_frame(node_spi, tx, rx);
	if (ret)
		goto out;

	/* The second transfer, at least MIN_GAP_US later (stm32_node_read_frame() waits), gets the
	 * frame loaded at the end of the first one: the newest the node has, at most one sensor
	 * period old. Only this frame is checked below. It costs a second transfer on every read
	 * (about 0.4 ms). If the node has just started and the sensor task has not made a frame yet,
	 * this is still the placeholder (status 1); the program sees that in the status byte. */
	ret = stm32_node_read_frame(node_spi, tx, rx);
	if (ret)
		goto out;

	if (rx[OFFSET_MARKER] != FRAME_MARKER) {
		/* A dead or missing node reads as all 00 or all FF, never 0xA5 (frame.h). */
		ret = -EIO;
		goto out;
	}
	/* The node stored its CRC in two bytes, low byte first (little-endian), so the 16-bit
	 * number is the first byte plus the second one shifted left by 8 bits. */
	if (crc16(rx, CRC_BYTES) != (rx[OFFSET_CRC] | (rx[OFFSET_CRC + 1] << 8))) {
		ret = -EIO;
		goto out;
	}

	/* The kernel must not write to buf directly: it is an address of another address space and
	 * may be invalid. copy_to_user() checks it and copies; it returns the number of bytes it
	 * could NOT copy, so any value other than 0 is an error. */
	if (copy_to_user(buf, rx, FRAME_SIZE)) {
		ret = -EFAULT;
		goto out;
	}
	ret = FRAME_SIZE;

out:
	/* Every exit goes through here, so the buffers are freed in one place. kfree(NULL) does
	 * nothing, which makes this safe when an allocation failed. */
	kfree(tx);
	kfree(rx);
	return ret;
}

/* The functions the kernel can call on /dev/stm32_sensor_node. Only read is filled in; the other
 * fields stay empty. .owner makes the kernel count the module as in use while the file is open,
 * so that rmmod refuses to remove the code under a read() that is running. */
static const struct file_operations stm32_node_fops = {
	.owner = THIS_MODULE,
	.read = stm32_node_read,
};

/* What makes the file: misc_register() creates /dev/stm32_sensor_node and links it to the table
 * above. MISC_DYNAMIC_MINOR lets the kernel choose the minor number, which only tells the kernel
 * which misc device a file is. */
static struct miscdevice stm32_node_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "stm32_sensor_node",
	.fops = &stm32_node_fops,
};

/*
 * Called by the kernel when it finds an SPI device that matches this driver: that is, when a
 * device-tree node with a compatible string from the table below exists on the bus. `spi`
 * describes that device: which chip select it is on, the maximum clock the node asked for, and
 * the SPI mode. The kernel has already read these from the device-tree node.
 *
 * A return value of 0 means "I accept this device". A negative error number means "no", and the
 * kernel then leaves the device without a driver.
 *
 * probe does not talk to the node. Whether the node is powered or connected is found out by each
 * read(), which returns -EIO if it is not; the node may be powered later, and the device exists on
 * the bus either way. Only a failed misc_register() makes probe fail.
 */
static int stm32_node_probe(struct spi_device *spi)
{
	/* The mode is printed as the SPI core set it. The value 4 is not a clock mode: bits 0 and 1
	 * (CPHA, CPOL) are 0, so the clock is mode 0, which the node expects. Bit 2 is SPI_CS_HIGH,
	 * set by the SPI core itself (drivers/spi/spi.c, "forcing CS_HIGH") when the controller
	 * drives the chip select through a GPIO descriptor, because the descriptor already holds
	 * the active-low polarity from the device tree. The line is still active low on the wire. */
	dev_info(&spi->dev, "probe: chip select %u, max clock %u Hz, mode %u\n",
		 spi_get_chipselect(spi, 0), spi->max_speed_hz, spi->mode);

	/* node_spi is set BEFORE the file exists: as soon as misc_register() returns, a program
	 * can call read(), and read() needs it. */
	node_spi = spi;
	return misc_register(&stm32_node_misc);
}

/* Called when the device goes away or the driver is unloaded: undo what probe did. The file is
 * removed first, so that no new read() starts after node_spi is cleared. */
static void stm32_node_remove(struct spi_device *spi)
{
	misc_deregister(&stm32_node_misc);
	node_spi = NULL;
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
