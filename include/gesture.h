#pragma once

#include <stdbool.h>
#include <stdint.h>

enum gesture_type {
	GESTURE_TYPE_NONE = 0,
	GESTURE_TYPE_HOLD,
	GESTURE_TYPE_PINCH,
	GESTURE_TYPE_SWIPE,
};

enum gesture_direction {
	GESTURE_DIRECTION_NONE = 0,
	GESTURE_DIRECTION_UP = 1 << 0,
	GESTURE_DIRECTION_DOWN = 1 << 1,
	GESTURE_DIRECTION_LEFT = 1 << 2,
	GESTURE_DIRECTION_RIGHT = 1 << 3,
	GESTURE_DIRECTION_INWARD = 1 << 4,
	GESTURE_DIRECTION_OUTWARD = 1 << 5,
	GESTURE_DIRECTION_CLOCKWISE = 1 << 6,
	GESTURE_DIRECTION_COUNTERCLOCKWISE = 1 << 7,
};

enum hotcorner {
	HOTCORNER_NONE = 0,
	HOTCORNER_TOPLEFT,
	HOTCORNER_TOPRIGHT,
	HOTCORNER_BOTTOMLEFT,
	HOTCORNER_BOTTOMRIGHT,
};

const char *gesture_type_string(enum gesture_type type);
const char *gesture_direction_string(enum gesture_direction direction);

#define GESTURE_FINGERS_ANY 0

typedef struct gesture_t {
	enum gesture_type type;
	uint8_t fingers;
	uint32_t directions;
} gesture_t;

char *gesture_parse(const char *input, gesture_t *output);
char *gesture_to_string(const gesture_t *gesture);
bool gesture_check(const gesture_t *target, enum gesture_type type, uint8_t fingers);
bool gesture_match(const gesture_t *target, const gesture_t *to_match, bool exact);
bool gesture_equal(const gesture_t *a, const gesture_t *b);
int8_t gesture_compare(const gesture_t *a, const gesture_t *b);

typedef struct gesture_tracker_t {
	enum gesture_type type;
	uint8_t fingers;
	double dx, dy;
	double scale;
	double rotation;
} gesture_tracker_t;

void gesture_tracker_begin(gesture_tracker_t *tracker, enum gesture_type type, uint8_t fingers);
bool gesture_tracker_check(gesture_tracker_t *tracker, enum gesture_type type);
void gesture_tracker_update(gesture_tracker_t *tracker, double dx, double dy, double scale,
	double rotation);
void gesture_tracker_cancel(gesture_tracker_t *tracker);
void gesture_tracker_end(gesture_tracker_t *tracker);
