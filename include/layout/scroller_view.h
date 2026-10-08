#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	int column_count;
	double *xs;
	double *widths;
	double view_width;
	double gap;
} scroller_view_t;

double scroller_view_offset_fit(const scroller_view_t *v, double cur_x, double col_x, double col_w);
double scroller_view_offset_centered(const scroller_view_t *v, double cur_x, double col_x,
	double col_w);
bool scroller_view_offset_bounds(const scroller_view_t *v, double anchor_x, double *out_min,
	double *out_max);
void scroller_view_offset_clamp(const scroller_view_t *v, double anchor_x, double *offset);
void scroller_view_layout(const double *resolved_widths, int column_count, double gap, double *xs,
	double *widths);
