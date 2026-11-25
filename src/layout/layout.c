#include "dim.h"
#include "ipc/helpers.h"
#include "layout/layout.h"
#include "layout/tree.h"
#include "output/output.h"
#include "transaction.h"
#include "tree.h"
#include "types.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

bool layout_command_target(ipc_args_t *a, const bool need_client, output_t **mon,
		desktop_t **desk) {
	desktop_t *d = ipc_focused_desk(a, mon);
	if (d == NULL)
		return false;

	if (need_client && (d->focus == NULL || d->focus->client == NULL)) {
		ipc_fail(a, "No focused client\n");
		return false;
	}

	*desk = d;
	return true;
}

void layout_command_commit(ipc_args_t *a, output_t *mon, desktop_t *desk, const char *msg) {
	arrange(mon, desk, true);
	ipc_ok(a, msg);
}

typedef struct layout_state_t {
	layout_t layout;
	void *data;
	void (*destroy)(void *data);
	struct wl_list link;
} layout_state_t;

void *layout_state_peek(const desktop_t *d, const layout_t layout) {
	if (d == NULL)
		return NULL;

	const layout_state_t *s;
	wl_list_for_each(s, &d->layout_states, link) {
		if (s->layout == layout)
			return s->data;
	}
	return NULL;
}

void *layout_state_get(desktop_t *d, const layout_t layout, const size_t size,
		void (*destroy)(void *state)) {
	if (d == NULL || size == 0)
		return NULL;

	layout_state_t *s;
	wl_list_for_each(s, &d->layout_states, link) {
		if (s->layout == layout)
			return s->data;
	}

	s = malloc(sizeof(*s));
	if (s == NULL)
		return NULL;

	s->data = calloc(1, size);
	if (s->data == NULL) {
		free(s);
		return NULL;
	}

	s->layout = layout;
	s->destroy = destroy;
	wl_list_insert(d->layout_states.prev, &s->link);
	return s->data;
}

void layout_state_drop(desktop_t *d, const layout_t layout) {
	if (d == NULL)
		return;

	layout_state_t *s, *tmp;
	wl_list_for_each_safe(s, tmp, &d->layout_states, link) {
		if (s->layout != layout)
			continue;

		wl_list_remove(&s->link);
		if (s->destroy != NULL)
			s->destroy(s->data);
		else
			free(s->data);
		free(s);
		return;
	}
}

void *layout_state_get_impl(desktop_t *d, const layout_impl_t *impl, const size_t size,
		void (*destroy)(void *state)) {
	const layout_t layout = layout_id_for(impl);
	if (layout == (layout_t)-1)
		return NULL;

	return layout_state_get(d, layout, size, destroy);
}

void *layout_state_peek_impl(const desktop_t *d, const layout_impl_t *impl) {
	const layout_t layout = layout_id_for(impl);
	if (layout == (layout_t)-1)
		return NULL;

	return layout_state_peek(d, layout);
}

void layout_state_drop_impl(desktop_t *d, const layout_impl_t *impl) {
	const layout_t layout = layout_id_for(impl);
	if (layout == (layout_t)-1)
		return;

	layout_state_drop(d, layout);
}

void layout_states_clear(desktop_t *d) {
	if (d == NULL)
		return;

	layout_state_t *s, *tmp;
	wl_list_for_each_safe(s, tmp, &d->layout_states, link) {
		wl_list_remove(&s->link);
		if (s->destroy != NULL)
			s->destroy(s->data);
		else
			free(s->data);
		free(s);
	}
}

void arrange(output_t *m, desktop_t *d, const bool use_transaction) {
	if (d == NULL)
		return;

	if (m != NULL) {
		const struct wlr_box rect = desktop_usable_area(m, d);

		const layout_impl_t *impl = layout_get_impl(d->layout);
		if (impl && impl->arrange)
			impl->arrange(m, d, rect);
	}

	if (use_transaction)
		transaction_commit_dirty();

	dim_update_output(m);
}

bool layout_init_client(output_t *m, desktop_t *d, client_t *c) {
	if (!d || !c)
		return false;

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl && impl->init_client)
		return impl->init_client(m, d, c);

	return false;
}

void layout_client_state_changed(output_t *m, desktop_t *d, node_t *n, client_state_t old_state) {
	if (d == NULL)
		return;

	const layout_impl_t *impl = layout_get_impl(d->layout);
	if (impl != NULL && impl->on_client_state != NULL)
		impl->on_client_state(m, d, n, old_state);
}

void layout_desktop_changed(output_t *m, desktop_t *d) {
	if (d == NULL)
		return;

	layout_client_state_changed(m, d, NULL, STATE_TILED);
}

void layout_set(desktop_t *d, const layout_t new_layout) {
	if (d == NULL || (size_t)new_layout >= layout_count() || layout_get_impl(new_layout) == NULL)
		return;

	const layout_t old_layout = d->layout;

	if (old_layout == new_layout) {
		layout_desktop_changed(d->output ? d->output : output_focused(), d);
		return;
	}

	d->user_layout = old_layout;
	output_t *m = d->output != NULL ? d->output : output_focused();

	// let the outgoing layout drop the per-desktop state it owns
	const layout_impl_t *old_impl = layout_get_impl(old_layout);
	if (m != NULL && old_impl != NULL && old_impl->leave != NULL)
		old_impl->leave(m, d);

	d->layout = new_layout;

	// let the incoming layout adopt the toplevels already on the desktop
	const layout_impl_t *impl = layout_get_impl(new_layout);
	if (m != NULL && impl != NULL && impl->enter != NULL)
		impl->enter(m, d);

	layout_desktop_changed(m, d);
}

void layout_toggle(desktop_t *d, const layout_t target) {
	if (d == NULL)
		return;

	layout_set(d, d->layout == target ? d->user_layout : target);
}

static const layout_impl_t *active_impl(const desktop_t *d) {
	if (d == NULL)
		return NULL;

	return layout_get_impl(d->layout);
}

void layout_on_focus(output_t *m, desktop_t *d, node_t *n) {
	const layout_impl_t *impl = active_impl(d);
	if (impl != NULL && impl->on_focus != NULL) {
		impl->on_focus(m, d, n);
		return;
	}

	// layouts without an on_focus hook show every leaf of the desktop
	if (d == NULL || d->root == NULL)
		return;

	FOR_EACH_LEAF(node, d->root) {
		if (node->client != NULL && !node_is_minimized(node))
			node->client->flags.shown = true;
	}
}

bool layout_has_floating_placement(const layout_t layout) {
	const layout_impl_t *impl = layout_get_impl(layout);
	return impl != NULL && impl->places_floating != NULL;
}

bool layout_places_floating(const layout_t layout, desktop_t *d, client_t *c) {
	const layout_impl_t *impl = layout_get_impl(layout);
	if (impl == NULL || impl->places_floating == NULL || d == NULL || c == NULL)
		return false;

	return impl->places_floating(d, c);
}

void layout_client_mapped(output_t *m, desktop_t *d, client_t *c, const bool focused) {
	const layout_impl_t *impl = active_impl(d);
	if (impl != NULL && impl->client_mapped != NULL)
		impl->client_mapped(m, d, c, focused);
}

void layout_client_unmapped(output_t *m, desktop_t *d, client_t *c) {
	const layout_impl_t *impl = active_impl(d);
	if (impl != NULL && impl->client_unmapped != NULL)
		impl->client_unmapped(m, d, c);
}

bool layout_focus_after_unmap(output_t *m, desktop_t *d, node_t *removed) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->focus_after_unmap == NULL)
		return false;

	return impl->focus_after_unmap(m, d, removed);
}

bool layout_resize(output_t *m, desktop_t *d, const direction_t dir, const double delta) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->resize == NULL)
		return false;

	return impl->resize(m, d, dir, delta);
}

bool layout_wants_visible(const desktop_t *d, const node_t *n) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->wants_visible == NULL)
		return true;

	return impl->wants_visible(d, n);
}

bool layout_borderless(const desktop_t *d, const client_t *c) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->borderless == NULL)
		return false;

	return impl->borderless(d, c);
}

bool layout_reports_maximized(const desktop_t *d, const client_t *c) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->reports_maximized == NULL)
		return false;

	return impl->reports_maximized(d, c);
}

int layout_window_gap(const desktop_t *d) {
	const layout_impl_t *impl = active_impl(d);
	if (impl != NULL && impl->window_gap != NULL)
		return impl->window_gap(d);

	return d != NULL ? compute_window_gap(d) : 0;
}

bool layout_single_visible(const desktop_t *d) {
	const layout_impl_t *impl = active_impl(d);
	return impl != NULL && impl->single_visible;
}

bool layout_resize_edges(const desktop_t *d, const node_t *n, uint32_t *edges) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->resize_edges == NULL || edges == NULL)
		return false;

	return impl->resize_edges(d, n, edges);
}

bool layout_pointer_resize_begin(output_t *m, desktop_t *d, node_t *n, double *initial) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->pointer_resize_begin == NULL)
		return false;

	return impl->pointer_resize_begin(m, d, n, initial);
}

bool layout_pointer_resize_update(output_t *m, desktop_t *d, node_t *n, const double delta) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->pointer_resize_update == NULL)
		return false;

	return impl->pointer_resize_update(m, d, n, delta);
}

double layout_focus_scroll_distance(const desktop_t *d, const node_t *n) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->focus_scroll_distance == NULL)
		return 0.0;

	return impl->focus_scroll_distance(d, n);
}

uint32_t layout_pan_fingers(const desktop_t *d) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->pan_fingers == NULL)
		return 0;

	return impl->pan_fingers(d);
}

bool layout_pan_update(output_t *m, desktop_t *d, const double delta) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->pan_update == NULL)
		return false;

	return impl->pan_update(m, d, delta);
}

bool layout_pan_end(output_t *m, desktop_t *d) {
	const layout_impl_t *impl = active_impl(d);
	if (impl == NULL || impl->pan_end == NULL)
		return false;

	return impl->pan_end(m, d);
}
