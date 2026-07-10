#include "input.h"

#include "ipc.h"
#include "ipc_cmd.h"
#include "ipc_helpers.h"

#include <stdio.h>
#include <string.h>

void ipc_cmd_input(char **args, int num, int client_fd) {
	if (num < 1) {
		send_failure(client_fd, "input: missing arguments\n");
		return;
	}

	char *identifier = NULL;
	enum input_config_type type = INPUT_CONFIG_TYPE_ANY;
	if (num > 0 && strncmp(*args, "type:", 5) != 0) {
		identifier = *args;
		args++;
		num--;
	}

	if (num > 0 && strncmp(*args, "type:", 5) == 0) {
		char *type_str = *args + 5;
		while (*type_str == ' ')
			type_str++;

		if (streq(type_str, "keyboard")) {
			type = INPUT_CONFIG_TYPE_KEYBOARD;
		} else if (streq(type_str, "pointer")) {
			type = INPUT_CONFIG_TYPE_POINTER;
		} else if (streq(type_str, "touchpad")) {
			type = INPUT_CONFIG_TYPE_TOUCHPAD;
		} else if (streq(type_str, "touchscreen")) {
			type = INPUT_CONFIG_TYPE_TOUCH;
		} else if (streq(type_str, "tablet")) {
			type = INPUT_CONFIG_TYPE_TABLET;
		} else if (streq(type_str, "tablet_pad")) {
			type = INPUT_CONFIG_TYPE_TABLET_PAD;
		} else if (streq(type_str, "switch")) {
			type = INPUT_CONFIG_TYPE_SWITCH;
		} else if (streq(type_str, "any")) {
			type = INPUT_CONFIG_TYPE_ANY;
		} else {
			send_failure(client_fd, "input: unknown type\n");
			return;
		}

		args++;
		num--;
	}

	if (num < 1) {
		send_failure(client_fd, "input: missing property\n");
		return;
	}

	char *property = *args;
	args++;
	num--;

	char *value = "";
	if (num > 0)
		value = *args;

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
			send_failure(client_fd, "input: failed to create config\n");
			return;
		}
		config->type = type;
		input_config_add(config);
	}

	if (!input_config_set_value(config, property, value)) {
		send_failure(client_fd, "input: unknown property\n");
		return;
	}

	if (!config) {
		send_failure(client_fd, "input: failed to create config\n");
		return;
	}
	config->type = type;

	if (!input_config_set_value(config, property, value)) {
		input_config_destroy(config);
		send_failure(client_fd, "input: unknown property\n");
		return;
	}

	input_apply_config_all_pointers();
	input_apply_config_all_keyboards();
}
