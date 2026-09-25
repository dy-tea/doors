#pragma once

#include "types.h"

extern doors_settings_t settings;

void refresh_border_color_cache(void);

extern struct output_t *mon;
extern struct wl_list mon_list;
extern struct wl_list orphan_desk_list;
extern uint32_t next_node_id;
extern uint32_t next_desktop_id;
extern uint32_t next_monitor_id;
