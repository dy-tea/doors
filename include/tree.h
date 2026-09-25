#pragma once

#include "borders.h"
#include "client.h"
#include "tree_focus.h"
#include "tree_layout.h"
#include "types.h"

#include <wlr/types/wlr_scene.h>

typedef struct surface_rounded_t surface_rounded_t;
typedef struct output_t output_t;

// node creation and destruction
node_t *make_node(uint32_t id);
client_t *make_client(void);
void free_node(node_t *n);

// node insertion and removal
node_t *find_public(desktop_t *d);
node_t *insert_node(desktop_t *d, node_t *n, node_t *f);
void remove_node(desktop_t *d, node_t *n);
void kill_node(desktop_t *d, node_t *n);

// node queries
bool is_leaf(node_t *n);
bool is_tiled(client_t *c);
bool is_floating(client_t *c);
bool is_first_child(node_t *n);

static inline bool node_is_detached(node_t *n) {
	return n != NULL && is_floating(n->client);
}

node_t *first_extrema(node_t *n);
node_t *second_extrema(node_t *n);
node_t *next_leaf(node_t *n, node_t *r);
node_t *prev_leaf(node_t *n, node_t *r);

#define FOR_EACH_LEAF(it, r) \
	for (node_t *it = first_extrema(r); it != NULL; it = next_leaf(it, r))
#define FOR_EACH_LEAF_SANS_ROOT(it, r) \
	for (node_t *it = first_extrema(r); it != NULL && it != (r); it = next_leaf(it, r))

// node manipulation
void swap_nodes(struct output_t *m1, desktop_t *d1, node_t *n1, struct output_t *m2, desktop_t *d2,
	node_t *n2);
int collect_tiled_leaves(desktop_t *d, node_t ***out_nodes);
bool set_state(struct output_t *m, desktop_t *d, node_t *n, client_state_t s);
void client_set_fullscreen(struct output_t *m, desktop_t *d, node_t *n, bool value);
bool client_set_maximized(struct output_t *m, desktop_t *d, node_t *n, bool value);
bool client_is_maximized(const client_t *c);
bool client_reports_maximized(const client_t *c, desktop_t *d);
bool node_is_minimized(const node_t *n);
bool client_set_minimized(struct output_t *m, desktop_t *d, node_t *n, bool value);

// preselection
presel_t *make_presel(void);
void presel_dir(node_t *n, direction_t dir);
void presel_cancel(node_t *n);

// tree transformations
void rotate_tree(node_t *n, int deg);
void flip_tree(node_t *n, flip_t flp);
void equalize_tree(node_t *n);
void balance_tree(node_t *n);

// Transaction helpers
void node_set_dirty(node_t *n);
void node_set_pending_rectangle(node_t *n, struct wlr_box rect);
void node_set_hidden(node_t *n, bool hidden);

// Debug helpers
void validate_tree(const char *context, desktop_t *d);

// macros for state checking
#define IS_TILED(c) (is_tiled(c))
#define IS_FLOATING(c) (is_floating(c))
#define IS_RECEPTACLE(n) ((n) != NULL && (n)->client == NULL && is_leaf(n))
