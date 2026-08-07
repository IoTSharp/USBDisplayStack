# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
BUILD_DIR ?= $(abspath ../build)
CPPFLAGS += -I../include
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC

DAEMON := $(BUILD_DIR)/usb-displayd
NULL_BACKEND := $(BUILD_DIR)/usbdisplay-null.so
PPM_BACKEND := $(BUILD_DIR)/usbdisplay-ppm.so
ACTIONS_MICRO_BACKEND := $(BUILD_DIR)/usbdisplay-actions-micro.so

.PHONY: all clean

all: $(DAEMON) $(NULL_BACKEND) $(PPM_BACKEND) $(ACTIONS_MICRO_BACKEND)

$(BUILD_DIR):
	mkdir -p $@

$(DAEMON): usb-displayd.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ -ldl

$(NULL_BACKEND): ../backends/null/null_backend.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $< -o $@

$(PPM_BACKEND): ../backends/ppm/ppm_backend.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $< -o $@

$(ACTIONS_MICRO_BACKEND): \
	../backends/actions-micro/actions_micro_backend.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared $< -o $@

clean:
	rm -f $(DAEMON) $(NULL_BACKEND) $(PPM_BACKEND) \
		$(ACTIONS_MICRO_BACKEND)
