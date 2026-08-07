# SPDX-License-Identifier: GPL-2.0-only

ifneq ($(KERNELRELEASE),)
obj-m += usbdisplay.o
usbdisplay-y := usbdisplay_drv.o
ccflags-y += -I$(src)/../include
else
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
endif
