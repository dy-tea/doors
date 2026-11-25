#pragma once

#include "settings.h"
#include "layout/scroller.h"
#include <stdbool.h>

// scroller layout settings
typedef struct {
	bool always_center_single_column;
	scroller_center_mode_t center_focused_column;
	int strut_left, strut_right, strut_top, strut_bottom;
	int pan_fingers;
	int snap_percent;
	scroller_size_t default_column_width;
	scroller_size_t *preset_column_widths;
	int preset_column_widths_count;
	scroller_size_t *preset_window_heights;
	int preset_window_heights_count;
} scroller_config_t;

extern scroller_config_t scroller_config;

#define SCROLLER_SETTINGS_COUNT 8
extern const setting_t scroller_settings[SCROLLER_SETTINGS_COUNT];
extern const command_t scroller_commands[];

// restores the values the layout keeps outside its setting table
void scroller_reset_settings(void);
