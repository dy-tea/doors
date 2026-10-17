#include "once.h"
#include "protocol/copy_capture.h"
#include "protocol/screencopy.h"
#include "protocol/session_mgmt.h"
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

#define PRIVILEGED_GLOBALS(X) \
	X(server.output_manager->global) \
	X(server.output_power_manager->global) \
	X(server.input_method_manager->global) \
	X(server.foreign_toplevel_list->global) \
	X(server.foreign_toplevel_manager->global) \
	X(server.data_control_manager->global) \
	X(server.ext_data_control_manager->global) \
	X(server.export_dmabuf_manager->global) \
	X(server.gamma_control_manager->global) \
	X(server.security_context_manager_v1->global) \
	X(server.layer_shell->global) \
	X(server.session_lock_manager->global) \
	X(server.keyboard_shortcuts_inhibit_manager->global) \
	X(server.virtual_keyboard_manager->global) \
	X(server.virtual_pointer_manager->global) \
	X(server.xdg_output_manager->global) \
	X(server.workspace_manager->global) \
	X(screencopy_get_global()) \
	X(image_capture_source_get_global()) \
	X(image_copy_capture_get_global()) \
	X(session_mgr_get_global())

#define PRIVILEGED_COUNT_ONE(...) + 1
enum {
	PRIVILEGED_COUNT = 0 PRIVILEGED_GLOBALS(PRIVILEGED_COUNT_ONE)
};

static struct wl_global *privileged_globals[PRIVILEGED_COUNT];
static size_t privileged_count = 0;
static bool privileged_resolved = false;

static void resolve_privileged_globals(void) {
	if (privileged_resolved)
		return;

#define PRIV(p) do { \
		struct wl_global *priv_global = (p); \
		if (priv_global) \
			privileged_globals[privileged_count++] = priv_global; \
	} while (0);

	PRIVILEGED_GLOBALS(PRIV)

#undef PRIV

	privileged_resolved = true;
}

static bool is_privileged(const struct wl_global *global) {
	resolve_privileged_globals();

	for (size_t i = 0; i < privileged_count; i++) {
		if (privileged_globals[i] == global)
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
