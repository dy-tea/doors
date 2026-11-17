#pragma once

#include "layout.h"

// directional focus over the split tree, shared by the tiled and monocle layouts
bool tiled_focus(desktop_t *d, direction_t dir);

// collects the tiled leaves of the split tree, shared by the tiled and monocle
// layouts
int tiled_collect(desktop_t *d, node_t ***out_nodes);
