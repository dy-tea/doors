#pragma once

#include "types.h"

void render_unfocused_init(void);
void render_unfocused_fini(void);
void render_unfocused_client_update(client_t *client);
void render_unfocused_client_remove(client_t *client);
