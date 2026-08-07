# SPDX-License-Identifier: GPL-2.0-only

CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
BUILD_DIR ?= $(abspath ../../build/examples)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror
LVGL_BUILD_CFLAGS ?= -O2 -g -std=gnu11

ifneq ($(LVGL_PATH),)
LVGL_CFLAGS := -I$(LVGL_PATH) -DLV_CONF_SKIP=1
LVGL_STATIC := $(BUILD_DIR)/liblvgl.a
LVGL_SOURCES := $(shell find $(LVGL_PATH)/src -type f -name '*.c')
LVGL_OBJECTS := $(patsubst $(LVGL_PATH)/src/%.c,\
	$(BUILD_DIR)/lvgl/%.o,$(LVGL_SOURCES))
LVGL_LIBS := $(LVGL_STATIC) -lm
LVGL_DEPENDENCY := $(LVGL_STATIC)
else
LVGL_CFLAGS := $(shell $(PKG_CONFIG) --cflags lvgl 2>/dev/null)
LVGL_LIBS := $(shell $(PKG_CONFIG) --libs lvgl 2>/dev/null)
LVGL_DEPENDENCY :=
endif

DRM_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null)
DRM_LIBS := $(shell $(PKG_CONFIG) --libs libdrm 2>/dev/null)

FBDEV_EXAMPLE := $(BUILD_DIR)/lvgl-fbdev-example
DRM_EXAMPLE := $(BUILD_DIR)/lvgl-drm-example

.PHONY: all clean

all: $(FBDEV_EXAMPLE) $(DRM_EXAMPLE)

$(BUILD_DIR):
	mkdir -p $@

ifneq ($(LVGL_PATH),)
$(BUILD_DIR)/lvgl/%.o: $(LVGL_PATH)/src/%.c
	mkdir -p $(dir $@)
	$(CC) $(LVGL_BUILD_CFLAGS) $(LVGL_CFLAGS) -c $< -o $@

$(LVGL_STATIC): $(LVGL_OBJECTS) | $(BUILD_DIR)
	$(AR) rcs $@ $(LVGL_OBJECTS)
endif

$(FBDEV_EXAMPLE): fbdev/main.c common/demo_ui.h $(LVGL_DEPENDENCY) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LVGL_CFLAGS) $< -o $@ $(LVGL_LIBS)

$(DRM_EXAMPLE): drm/main.c common/demo_ui.h $(LVGL_DEPENDENCY) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LVGL_CFLAGS) $(DRM_CFLAGS) $< -o $@ \
		$(LVGL_LIBS) $(DRM_LIBS)

clean:
	rm -f $(FBDEV_EXAMPLE) $(DRM_EXAMPLE) $(LVGL_STATIC)
	rm -rf $(BUILD_DIR)/lvgl
