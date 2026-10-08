#pragma once

#include "types.h"

extern doors_settings_t settings;

void settings_init(void);
void refresh_border_color_cache(void);
void settings_fini(void);

void settings_set_scroller_preset_column_widths(scroller_size_t *list, int count);
void settings_set_scroller_preset_window_heights(scroller_size_t *list, int count);

bool config_apply_value(const char *name, const char *value, char *err, size_t errsz);

extern struct output_t *mon;
extern struct wl_list mon_list;
extern struct wl_list orphan_desk_list;
extern uint32_t next_node_id;
extern uint32_t next_desktop_id;
extern uint32_t next_monitor_id;
