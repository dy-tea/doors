#pragma once

#include <stdbool.h>

// resolved config paths
const char *config_doorsrc_path(void);
const char *config_hotkeys_path(void);

void hotkey_watch_fini(void);
