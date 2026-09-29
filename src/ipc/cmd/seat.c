#include "config.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "ipc/args.h"
#include "ipc/ipc.h"
#include "ipc/registry.h"
#include "server.h"

static void seat_list(ipc_args_t *a) {
	char buf[DOORS_BUFSIZ];
	ipc_buf_t b;
	ipc_buf_init(&b, buf, sizeof(buf));

	seat_t *s;
	wl_list_for_each(s, &server.seats, link) {
		ipc_buff(&b, "%s", s->name);
		if (s == seat_default())
			ipc_buff(&b, " (default)");
		ipc_buff(&b, "\n");
	}
	if (b.len == 0)
		ipc_buff(&b, "No seats\n");

	ipc_buf_send(a, &b);
}

const ipc_sub_t seat_subs[] = {
	IPC_SUB("list", NULL, "seat list", seat_list),
	IPC_SUB_END,
};

void ipc_cmd_seat(ipc_args_t *a) {
	if (!ipc_sub_dispatch(a, seat_subs))
		ipc_fail_unknown(a, seat_subs);
}

static const cfg_enum_value_t grouping_values[] = {
	{"none", KEYBOARD_GROUP_NONE},
	{"smart", KEYBOARD_GROUP_SMART},
	{"default", KEYBOARD_GROUP_DEFAULT},
	IPC_ENUM_END,
};

void ipc_cmd_keyboard_grouping(ipc_args_t *a) {
	long mode;
	if (!ipc_enum(a, "mode", grouping_values, &mode))
		return;

	set_keyboard_grouping((keyboard_grouping_t)mode);
	keyboard_reapply_grouping();
	ipc_ok(a, "keyboard_grouping set\n");
}
