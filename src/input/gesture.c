#include "input/gesture.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

const char *gesture_type_string(enum gesture_type type) {
	switch (type) {
	case GESTURE_TYPE_NONE:
		return "none";
	case GESTURE_TYPE_HOLD:
		return "hold";
	case GESTURE_TYPE_PINCH:
		return "pinch";
	case GESTURE_TYPE_SWIPE:
		return "swipe";
	}
	return "???";
}

const char *gesture_direction_string(enum gesture_direction direction) {
	switch (direction) {
	case GESTURE_DIRECTION_UP:
		return "up";
	case GESTURE_DIRECTION_DOWN:
		return "down";
	case GESTURE_DIRECTION_LEFT:
		return "left";
	case GESTURE_DIRECTION_RIGHT:
		return "right";
	case GESTURE_DIRECTION_INWARD:
		return "inward";
	case GESTURE_DIRECTION_OUTWARD:
		return "outward";
	case GESTURE_DIRECTION_CLOCKWISE:
		return "clockwise";
	case GESTURE_DIRECTION_COUNTERCLOCKWISE:
		return "counterclockwise";
	case GESTURE_DIRECTION_NONE:
		break;
	}
	return "none";
}

static enum gesture_type gesture_parse_type(const char *type_str) {
	if (strcasecmp(type_str, "hold") == 0)
		return GESTURE_TYPE_HOLD;
	else if (strcasecmp(type_str, "pinch") == 0)
		return GESTURE_TYPE_PINCH;
	else if (strcasecmp(type_str, "swipe") == 0)
		return GESTURE_TYPE_SWIPE;
	return GESTURE_TYPE_NONE;
}

static enum gesture_direction gesture_parse_direction(const char *dir_str) {
	if (strcasecmp(dir_str, "up") == 0) {
		return GESTURE_DIRECTION_UP;
	} else if (strcasecmp(dir_str, "down") == 0) {
		return GESTURE_DIRECTION_DOWN;
	} else if (strcasecmp(dir_str, "left") == 0) {
		return GESTURE_DIRECTION_LEFT;
	} else if (strcasecmp(dir_str, "right") == 0) {
		return GESTURE_DIRECTION_RIGHT;
	} else if (strcasecmp(dir_str, "in") == 0 || strcasecmp(dir_str, "inward") == 0) {
		return GESTURE_DIRECTION_INWARD;
	} else if (strcasecmp(dir_str, "out") == 0 || strcasecmp(dir_str, "outward") == 0) {
		return GESTURE_DIRECTION_OUTWARD;
	} else if (strcasecmp(dir_str, "clockwise") == 0) {
		return GESTURE_DIRECTION_CLOCKWISE;
	} else if (strcasecmp(dir_str, "counterclockwise") == 0) {
		return GESTURE_DIRECTION_COUNTERCLOCKWISE;
	}
	return GESTURE_DIRECTION_NONE;
}

char *gesture_parse(const char *input, gesture_t *output) {
	if (!input || !output)
		return "invalid input";

	memset(output, 0, sizeof(gesture_t));

	char *input_copy = strdup(input);
	if (!input_copy)
		return "memory allocation failed";

	char *saveptr = NULL;
	char *token = strtok_r(input_copy, ":", &saveptr);
	if (!token) {
		free(input_copy);
		return "missing gesture type";
	}

	output->type = gesture_parse_type(token);
	if (output->type == GESTURE_TYPE_NONE) {
		free(input_copy);
		return "unknown gesture type";
	}

	token = strtok_r(NULL, ":", &saveptr);
	if (token) {
		if (strcmp(token, "*") == 0) {
			output->fingers = GESTURE_FINGERS_ANY;
		} else {
			int fingers = atoi(token);
			if (fingers < 1 || fingers > 5) {
				enum gesture_direction dir = gesture_parse_direction(token);
				if (dir == GESTURE_DIRECTION_NONE) {
					free(input_copy);
					return "invalid finger count";
				}
				output->directions |= dir;
			} else {
				output->fingers = (uint8_t)fingers;
			}
		}

		token = strtok_r(NULL, ":", &saveptr);
		if (token) {
			char *dirs = strdup(token);
			if (!dirs) {
				free(input_copy);
				return "memory allocation failed";
			}

			char *dir_saveptr = NULL;
			char *dir_token = strtok_r(dirs, "+", &dir_saveptr);
			while (dir_token) {
				enum gesture_direction dir = gesture_parse_direction(dir_token);
				if (dir != GESTURE_DIRECTION_NONE)
					output->directions |= dir;
				dir_token = strtok_r(NULL, "+", &dir_saveptr);
			}
			free(dirs);
		}
	}

	if (output->fingers == 0 && output->type == GESTURE_TYPE_PINCH)
		output->fingers = 2;

	free(input_copy);
	return NULL;
}

bool gesture_check(const gesture_t *target, enum gesture_type type, uint8_t fingers) {
	if (!target || target->type != type)
		return false;
	if (target->fingers != GESTURE_FINGERS_ANY && target->fingers != fingers)
		return false;

	return true;
}

int8_t gesture_compare(const gesture_t *a, const gesture_t *b) {
	if (!a || !b)
		return -1;
	if (a->type != b->type)
		return -1;

	if (a->fingers != b->fingers)
		return (a->fingers > b->fingers) ? 1 : -1;

	int a_bits = __builtin_popcount(a->directions);
	int b_bits = __builtin_popcount(b->directions);

	if (a_bits == b_bits)
		return 0;

	return (a_bits > b_bits) ? 1 : -1;
}

void gesture_tracker_begin(gesture_tracker_t *tracker, enum gesture_type type, uint8_t fingers) {
	if (!tracker)
		return;

	tracker->type = type;
	tracker->fingers = fingers;
	tracker->dx = 0;
	tracker->dy = 0;
	tracker->scale = 1.0;
	tracker->rotation = 0;
}

bool gesture_tracker_check(gesture_tracker_t *tracker, enum gesture_type type) {
	if (!tracker)
		return false;

	return tracker->type == type && tracker->fingers > 0;
}

void gesture_tracker_update(gesture_tracker_t *tracker, double dx, double dy, double scale,
		double rotation) {
	if (!tracker)
		return;

	tracker->dx += dx;
	tracker->dy += dy;

	if (!isnan(scale))
		tracker->scale = scale;
	if (!isnan(rotation))
		tracker->rotation = rotation;
}

void gesture_tracker_cancel(gesture_tracker_t *tracker) {
	if (!tracker)
		return;

	tracker->type = GESTURE_TYPE_NONE;
	tracker->fingers = 0;
	tracker->dx = 0;
	tracker->dy = 0;
	tracker->scale = 1.0;
	tracker->rotation = 0;
}

void gesture_tracker_end(gesture_tracker_t *tracker) {
	if (!tracker)
		return;

	tracker->type = GESTURE_TYPE_NONE;
	tracker->fingers = 0;
}
