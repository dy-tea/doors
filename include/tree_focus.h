#pragma once

#include "types.h"

typedef struct output_t output_t;
typedef struct desktop_t desktop_t;
typedef struct node_t node_t;

// true if the node still has a live surface behind it and can take focus
bool node_focusable(node_t *n);

// the node to focus when the current one goes away: the topmost floating
// toplevel on this desktop, else the first leaf in the tree
node_t *desktop_fallback_focus(desktop_t *d, node_t *skip);

bool focus_node(output_t *m, desktop_t *d, node_t *n);

// focus depending on settings.focus_on_activate, may not hand over the keyboard
bool activate_node(output_t *m, desktop_t *d, node_t *n);

// the node on the other side of the first split in `dir` between n and the root
node_t *find_fence(node_t *n, direction_t dir);
