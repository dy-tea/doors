#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#define BEZIER_BAKED_POINTS 256

typedef struct bezier_curve_t {
	struct wl_list link;
	char name[64];
	double p1x, p1y, p2x, p2y;

	struct {
		float x, y;
	} baked[BEZIER_BAKED_POINTS];
} bezier_curve_t;

void bezier_init(void);
void bezier_fini(void);

bezier_curve_t *bezier_add(const char *name, double p1x, double p1y, double p2x, double p2y);
bool bezier_exists(const char *name);
bezier_curve_t *bezier_find(const char *name);
double bezier_evaluate(const char *name, double x);
double bezier_evaluate_curve(const bezier_curve_t *curve, double x);
