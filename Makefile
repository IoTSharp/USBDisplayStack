# SPDX-License-Identifier: GPL-2.0-only

BUILD_DIR ?= $(CURDIR)/build

.PHONY: all userspace module examples-lvgl check clean

all: userspace

userspace:
	$(MAKE) -C userspace BUILD_DIR=$(BUILD_DIR)
	$(MAKE) -C tools BUILD_DIR=$(BUILD_DIR)

module:
	$(MAKE) -C kernel

examples-lvgl:
	$(MAKE) -C examples/lvgl BUILD_DIR=$(BUILD_DIR)/examples

check:
	sh tests/test-usbdisplay-check.sh
	sh tests/test-package-deb.sh
	sh tests/test-splash-preview.sh

clean:
	$(MAKE) -C userspace BUILD_DIR=$(BUILD_DIR) clean
	$(MAKE) -C tools BUILD_DIR=$(BUILD_DIR) clean
	$(MAKE) -C examples/lvgl BUILD_DIR=$(BUILD_DIR)/examples clean
	$(MAKE) -C kernel clean
