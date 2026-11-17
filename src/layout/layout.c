#include "dim.h"
#include "layout/layout.h"
#include "layout/tree.h"
#include "output/output.h"
#include "transaction.h"
#include "types.h"
#include <wlr/util/box.h>

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
