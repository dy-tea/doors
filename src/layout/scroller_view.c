#include "layout/scroller_view.h"
#include "macros.h"
#include <math.h>

double scroller_view_offset_fit(const scroller_view_t *v, const double cur_x, const double col_x,
		const double col_w) {
	if (v->view_width <= col_w)
		return 0.0;

	const double padding = CLAMP((v->view_width - col_w) / 2.0, 0.0, v->gap);

	const double left_x = col_x - padding;
	const double right_x = col_x + col_w + padding;

	if (cur_x <= left_x && right_x <= cur_x + v->view_width)
		return cur_x - col_x;

	const double dist_to_left = fabs(cur_x - left_x);
	const double dist_to_right = fabs(cur_x + v->view_width - right_x);

	return dist_to_left <= dist_to_right ? -padding : -(v->view_width - padding - col_w);
}

double scroller_view_offset_edge(const scroller_view_t *v, const double cur_x, const double col_x,
		double col_w) {
	if (v->view_width <= col_w)
		return scroller_view_offset_fit(v, cur_x, col_x, col_w);

	const double padding = CLAMP((v->view_width - col_w) / 2.0, 0.0, v->gap);
	const double left = -padding;
	const double right = -(v->view_width - padding - col_w);

	return fabs(cur_x - (col_x + left)) <= fabs(cur_x - (col_x + right)) ? left : right;
}

double scroller_view_offset_centered(const scroller_view_t *v, const double cur_x,
		const double col_x, const double col_w) {
	if (v->view_width <= col_w)
		return scroller_view_offset_fit(v, cur_x, col_x, col_w);

	return -(v->view_width - col_w) / 2.0;
}

bool scroller_view_offset_bounds(const scroller_view_t *v, const double anchor_x, double *out_min,
		double *out_max) {
	const double min_offset = -anchor_x;
	const double max_offset = v->xs[v->column_count] - anchor_x - v->view_width;

	if (max_offset < min_offset)
		return false;

	if (out_min)
		*out_min = min_offset;
	if (out_max)
		*out_max = max_offset;
	return true;
}

void scroller_view_offset_clamp(const scroller_view_t *v, const double anchor_x, double *offset) {
	double min_offset, max_offset;
	if (!scroller_view_offset_bounds(v, anchor_x, &min_offset, &max_offset))
		return;

	*offset = CLAMP(*offset, min_offset, max_offset);
}

void scroller_view_layout(const double *resolved_widths, const int column_count, const double gap,
		double *xs, double *widths) {
	double wx = 0.0;
	for (int i = 0; i < column_count; i++) {
		xs[i] = wx;
		widths[i] = resolved_widths[i] > 0.0 ? resolved_widths[i] : 0.0;
		if (widths[i] > 0.0)
			wx += widths[i] + gap;
	}
	xs[column_count] = wx;
}
