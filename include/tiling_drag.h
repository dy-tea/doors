#pragma once

#include "types.h"

#include <stdbool.h>

#define TILING_DRAG_THRESHOLD 5

void tiling_drag_begin(node_t *node);
void tiling_drag_motion(void);
void tiling_drag_finish(void);
void tiling_drag_abort(void);
