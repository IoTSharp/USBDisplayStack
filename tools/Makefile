# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
BUILD_DIR ?= $(abspath ../build)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

TOOLS := $(BUILD_DIR)/fb-test-pattern $(BUILD_DIR)/drm-probe \
	$(BUILD_DIR)/actions-micro-replay

.PHONY: all clean

all: $(TOOLS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/fb-test-pattern: fb-test-pattern.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/drm-probe: drm-probe.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/actions-micro-replay: actions-micro-replay.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TOOLS)
