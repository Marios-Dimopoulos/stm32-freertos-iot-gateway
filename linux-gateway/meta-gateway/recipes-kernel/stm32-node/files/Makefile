# Out-of-tree kernel module. This file is not run by itself: the `module` class of Yocto calls
# `make` with KERNEL_SRC (the kernel source) and O= (the kernel build folder) already set.
#
# obj-m means "build stm32_node.o as a loadable module", which gives stm32_node.ko.
obj-m := stm32_node.o

SRC := $(shell pwd)

# The kernel's own build system does the compiling, so we call it with M= pointing to our folder.
all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean
