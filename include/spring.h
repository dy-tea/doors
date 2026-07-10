#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>

#define SPRING_EPSILON_DEFAULT 0.001

typedef struct {
	char name[64];
	double stiffness;
	double damping;
	double mass;
	double value_epsilon;
	double velocity_epsilon;
	struct wl_list link;
} spring_curve_t;

void spring_init(void);
void spring_fini(void);

spring_curve_t *spring_add(
    const char *name, double stiffness, double damping, double mass, double value_epsilon, double velocity_epsilon);
bool spring_exists(const char *name);
spring_curve_t *spring_find(const char *name);

double spring_evaluate(const spring_curve_t *curve, double dt, double *position, double *velocity, bool *done);
