/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USBDISPLAY_LVGL_DEMO_UI_H
#define USBDISPLAY_LVGL_DEMO_UI_H

#include <stdint.h>
#include <time.h>

#include <lvgl.h>

static uint32_t usbdisplay_demo_ticks(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}

	return (uint32_t)((uint64_t)now.tv_sec * 1000ULL +
			  (uint64_t)now.tv_nsec / 1000000ULL);
}

struct usbdisplay_demo_state {
	lv_obj_t *load_bar;
	lv_obj_t *temperature_label;
	lv_obj_t *frame_label;
	unsigned int frame;
};

static void usbdisplay_demo_tick(lv_timer_t *timer)
{
	struct usbdisplay_demo_state *state = lv_timer_get_user_data(timer);
	unsigned int load = 18U + (state->frame * 7U) % 78U;
	unsigned int temperature = 36U + (state->frame / 3U) % 17U;

	lv_bar_set_value(state->load_bar, (int32_t)load, LV_ANIM_ON);
	lv_label_set_text_fmt(state->temperature_label, "%u C", temperature);
	lv_label_set_text_fmt(state->frame_label, "FRAME %05u", state->frame);
	++state->frame;
}

static lv_obj_t *usbdisplay_demo_metric(lv_obj_t *parent, const char *title,
					const char *value, lv_obj_t **value_label)
{
	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_t *label;

	lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_grow(panel, 1);
	lv_obj_set_height(panel, LV_PCT(100));
	lv_obj_set_style_radius(panel, 6, 0);
	lv_obj_set_style_border_width(panel, 1, 0);
	lv_obj_set_style_border_color(panel, lv_color_hex(0x303b4d), 0);
	lv_obj_set_style_bg_color(panel, lv_color_hex(0x151b26), 0);
	lv_obj_set_style_pad_all(panel, 14, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	label = lv_label_create(panel);
	lv_label_set_text(label, title);
	lv_obj_set_style_text_color(label, lv_color_hex(0x8d99ab), 0);
	*value_label = lv_label_create(panel);
	lv_label_set_text(*value_label, value);
	lv_obj_set_style_text_color(*value_label, lv_color_hex(0xf4f7fb), 0);
#if defined(LV_FONT_MONTSERRAT_24) && LV_FONT_MONTSERRAT_24
	lv_obj_set_style_text_font(*value_label, &lv_font_montserrat_24, 0);
#endif

	return panel;
}

static void usbdisplay_create_demo_ui(void)
{
	static struct usbdisplay_demo_state state;
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *header;
	lv_obj_t *content;
	lv_obj_t *footer;
	lv_obj_t *label;
	lv_obj_t *status;
	lv_obj_t *load_panel;
	lv_obj_t *unused_value;

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x0d1118), 0);
	lv_obj_set_style_pad_all(screen, 20, 0);
	lv_obj_set_style_pad_gap(screen, 14, 0);
	lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

	header = lv_obj_create(screen);
	lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_width(header, LV_PCT(100));
	lv_obj_set_height(header, 54);
	lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(header, 0, 0);
	lv_obj_set_style_pad_all(header, 0, 0);
	lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	label = lv_label_create(header);
	lv_label_set_text(label, "USB DISPLAY / CONTROL");
	lv_obj_set_style_text_color(label, lv_color_hex(0xf4f7fb), 0);
#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
	lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
#endif
	status = lv_label_create(header);
	lv_label_set_text(status, "LIVE");
	lv_obj_set_style_text_color(status, lv_color_hex(0x64e6a5), 0);
	lv_obj_set_style_bg_color(status, lv_color_hex(0x16372d), 0);
	lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(status, 12, 0);
	lv_obj_set_style_pad_ver(status, 6, 0);
	lv_obj_set_style_radius(status, 4, 0);

	content = lv_obj_create(screen);
	lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_width(content, LV_PCT(100));
	lv_obj_set_flex_grow(content, 1);
	lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(content, 0, 0);
	lv_obj_set_style_pad_all(content, 0, 0);
	lv_obj_set_style_pad_gap(content, 14, 0);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);

	usbdisplay_demo_metric(content, "DEVICE", "185B:2D1D", &unused_value);
	usbdisplay_demo_metric(content, "THERMAL", "36 C",
			       &state.temperature_label);

	load_panel = lv_obj_create(content);
	lv_obj_remove_flag(load_panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_grow(load_panel, 2);
	lv_obj_set_height(load_panel, LV_PCT(100));
	lv_obj_set_style_radius(load_panel, 6, 0);
	lv_obj_set_style_border_width(load_panel, 1, 0);
	lv_obj_set_style_border_color(load_panel, lv_color_hex(0x303b4d), 0);
	lv_obj_set_style_bg_color(load_panel, lv_color_hex(0x151b26), 0);
	lv_obj_set_style_pad_all(load_panel, 14, 0);
	lv_obj_set_flex_flow(load_panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(load_panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	label = lv_label_create(load_panel);
	lv_label_set_text(label, "FRAME PIPELINE");
	lv_obj_set_style_text_color(label, lv_color_hex(0x8d99ab), 0);
	state.load_bar = lv_bar_create(load_panel);
	lv_obj_set_width(state.load_bar, LV_PCT(100));
	lv_bar_set_range(state.load_bar, 0, 100);
	lv_bar_set_value(state.load_bar, 18, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(state.load_bar, lv_color_hex(0x273142),
			       LV_PART_MAIN);
	lv_obj_set_style_bg_color(state.load_bar, lv_color_hex(0x4cc9a4),
			       LV_PART_INDICATOR);

	footer = lv_obj_create(screen);
	lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_width(footer, LV_PCT(100));
	lv_obj_set_height(footer, 42);
	lv_obj_set_style_bg_color(footer, lv_color_hex(0x101722), 0);
	lv_obj_set_style_border_width(footer, 0, 0);
	lv_obj_set_style_radius(footer, 4, 0);
	lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	label = lv_label_create(footer);
	lv_label_set_text(label, "XRGB8888 / 30 FPS");
	lv_obj_set_style_text_color(label, lv_color_hex(0x8d99ab), 0);
	state.frame_label = lv_label_create(footer);
	lv_label_set_text(state.frame_label, "FRAME 00000");
	lv_obj_set_style_text_color(state.frame_label, lv_color_hex(0x65a7ff), 0);

	lv_timer_create(usbdisplay_demo_tick, 100, &state);
}

#endif
