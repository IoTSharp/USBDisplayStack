# SPDX-License-Identifier: GPL-2.0-only

BUILD_DIR ?= $(CURDIR)/build

.PHONY: all userspace module clean

all: userspace

userspace:
	$(MAKE) -C userspace BUILD_DIR=$(BUILD_DIR)
	$(MAKE) -C tools BUILD_DIR=$(BUILD_DIR)

module:
	$(MAKE) -C kernel

clean:
	$(MAKE) -C userspace BUILD_DIR=$(BUILD_DIR) clean
	$(MAKE) -C tools BUILD_DIR=$(BUILD_DIR) clean
	$(MAKE) -C kernel clean
