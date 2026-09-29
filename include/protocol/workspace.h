#pragma once

#include <stdbool.h>
#include <wlr/types/wlr_ext_workspace_v1.h>

void workspace_init(void);
void workspace_fini(void);
void workspace_sync(void);
void workspace_create_desktop(const char *name);
void workspace_switch_to_desktop(const char *name);
void workspace_switch_to_desktop_by_index(int index);
void workspace_switch_to_last_desktop(void);
struct wlr_ext_workspace_handle_v1 *workspace_get_active(void);

typedef struct desktop_t desktop_t;
typedef struct output_t output_t;

desktop_t *find_desktop_by_name(const char *name);
desktop_t *find_desktop_by_name_in_monitor(output_t *mon, const char *name);

void desktop_init(desktop_t *d, output_t *output, const char *name);
