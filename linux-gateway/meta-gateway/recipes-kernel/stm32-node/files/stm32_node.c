// SPDX-License-Identifier: GPL-2.0-only
/*
 * stm32_node: driver for the STM32 sensor node on the Raspberry Pi's SPI bus.
 *
 * Step 4.2 of Phase 4: only the skeleton of a module. It does nothing but print a line when it
 * is loaded and one when it is unloaded, to prove that the whole path works: the recipe, the
 * build against the kernel of the image, the package, the image, `insmod` on the board. The SPI
 * driver (probe/remove), the character device and the frame checks come in the next steps.
 */
#include <linux/init.h>
#include <linux/module.h>

/* Puts the module name in front of every pr_*() line, so that `dmesg` shows "stm32_node: ...". */
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* Runs once, when the module is loaded (insmod, modprobe, or udev). Returning 0 means success;
 * a negative value would make the load fail. __init puts the function in memory that the kernel
 * frees after loading, because it is never needed again. */
static int __init stm32_node_init(void)
{
	pr_info("loaded\n");
	return 0;
}

/* Runs once, when the module is removed (rmmod). */
static void __exit stm32_node_exit(void)
{
	pr_info("unloaded\n");
}

module_init(stm32_node_init);
module_exit(stm32_node_exit);

/* The license is not decoration: a module that says it is not GPL "taints" the kernel and
 * cannot use the kernel functions that are exported to GPL modules only. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("STM32 sensor node on the SPI bus");
MODULE_AUTHOR("Marios-Georgios Dimopoulos");
