#include "once.h"
#include "protocol/copy_capture.h"
#include "protocol/screencopy.h"
#include "server.h"
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

static size_t collect_privileged_globals(struct wl_global **out, size_t max) {
#define PRIV(p) do { \
		if ((p) && (n) < max) \
			out[n++] = p; \
	} while (0)
	size_t n = 0;

	PRIV(server.output_manager->global);
	PRIV(server.output_power_manager->global);
	PRIV(server.input_method_manager->global);
	PRIV(server.foreign_toplevel_list->global);
	PRIV(server.foreign_toplevel_manager->global);
	PRIV(server.data_control_manager->global);
	PRIV(server.ext_data_control_manager->global);
	PRIV(server.export_dmabuf_manager->global);
	PRIV(server.gamma_control_manager->global);
	PRIV(server.security_context_manager_v1->global);
	PRIV(server.layer_shell->global);
	PRIV(server.session_lock_manager->global);
	PRIV(server.keyboard_shortcuts_inhibit_manager->global);
	PRIV(server.virtual_keyboard_manager->global);
	PRIV(server.virtual_pointer_manager->global);
	PRIV(server.xdg_output_manager->global);
	PRIV(server.workspace_manager->global);
	PRIV(screencopy_get_global());
	PRIV(image_capture_source_get_global());
	PRIV(image_capture_source_get_global());

#undef PRIV

	return n;
}

#define PRIVILEGED_MAX 24

static bool is_privileged(const struct wl_global *global) {
	struct wl_global *priv[PRIVILEGED_MAX];
	size_t n = collect_privileged_globals(priv, PRIVILEGED_MAX);

	for (size_t i = 0; i < n; i++) {
		if (priv[i] == global)
			return true;
	}

	return false;
}

static bool filter_global(const struct wl_client *client, const struct wl_global *global,
		void *data) {
	(void)data;
	const struct wlr_security_context_v1_state *security_context =
		wlr_security_context_manager_v1_lookup_client(server.security_context_manager_v1,
		(struct wl_client *)client);

	if (is_privileged(global))
		return security_context == NULL;

	return true;
}

void security_ctx_init(void) {
	ONCE();
	server.security_context_manager_v1 = wlr_security_context_manager_v1_create(server.wl_display);
	if (!server.security_context_manager_v1) {
		wlr_log(WLR_ERROR, "Failed to create security context manager");
		return;
	}

	wl_display_set_global_filter(server.wl_display, filter_global, NULL);
}

void security_ctx_fini(void) {
	ONCE();
}
