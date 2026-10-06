#include "input/cursor.h"
#include "input/input.h"
#include "ipc/args.h"
#include "ipc/registry.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const cfg_enum_value_t input_type_values[] = {
	{"keyboard", INPUT_CONFIG_TYPE_KEYBOARD},
	{"pointer", INPUT_CONFIG_TYPE_POINTER},
	{"touchpad", INPUT_CONFIG_TYPE_TOUCHPAD},
	{"touchscreen", INPUT_CONFIG_TYPE_TOUCH},
	{"tablet", INPUT_CONFIG_TYPE_TABLET},
	{"tablet_pad", INPUT_CONFIG_TYPE_TABLET_PAD},
	{"switch", INPUT_CONFIG_TYPE_SWITCH},
	{"any", INPUT_CONFIG_TYPE_ANY},
	IPC_ENUM_END,
};

void ipc_cmd_input(ipc_args_t *a) {
	const char *identifier = NULL;
	enum input_config_type type = INPUT_CONFIG_TYPE_ANY;

	const char *first = ipc_peek(a);
	if (first && strncmp(first, "type:", 5) != 0) {
		identifier = ipc_take(a);
	}

	if (ipc_peek(a) && strncmp(ipc_peek(a), "type:", 5) == 0) {
		const char *spec = ipc_take(a) + 5;
		while (*spec == ' ')
			spec++;

		long value;
		if (!ipc_enum_names(a, "type", spec, input_type_values, &value))
			return;

		type = (enum input_config_type)value;
	}

	const char *property;
	if (!ipc_need(a, "property", &property))
		return;

	const char *value = ipc_peek(a) ? ipc_peek(a) : "";

	if (strcmp(property, "xcursor_size") == 0) {
		char *end;
		errno = 0;
		long size = strtol(value, &end, 10);
		if (errno || value[0] == '\0' || end[0] != '\0' || size < 1 || size > 512) {
			ipc_fail(a, "input xcursor_size: expected 1-512\n");
			return;
		}
		if (!cursor_set_size((uint32_t)size)) {
			ipc_fail(a, "input xcursor_size: failed to load cursor theme\n");
			return;
		}
		setenv("XCURSOR_SIZE", value, true);
		ipc_ok(a, "ok\n");
		return;
	}

	input_config_t *config = NULL;
	for (size_t i = 0; i < num_input_configs; i++) {
		input_config_t *cfg = input_configs[i];
		if (identifier) {
			if (cfg->identifier && strcmp(cfg->identifier, identifier) == 0) {
				config = cfg;
				break;
			}
		} else if (cfg->type == type && cfg->identifier == NULL) {
			config = cfg;
			break;
		}
	}

	if (!config) {
		config = input_config_create(identifier);
		if (!config) {
			ipc_fail(a, "Failed to create config\n");
			return;
		}
		config->type = type;
		input_config_add(config);
	}

	if (!input_config_set_value(config, property, value)) {
		ipc_fail(a, "Unknown property \"%s\"\n", property);
		return;
	}

	input_apply_config_all_pointers();
	input_apply_config_all_keyboards();
	ipc_ok(a, "ok\n");
}
