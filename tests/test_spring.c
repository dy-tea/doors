#include "spring.h"
#include "test.h"

static double simulate(spring_curve_t *curve, double *out_velocity, double *out_overshoot) {
	double position = 0.0;
	double velocity = 0.0;
	bool done = false;
	double overshoot = 0.0;

	for (int step = 0; step < 2000; step++) {
		spring_evaluate(curve, 1.0 / 120.0, &position, &velocity, &done);
		if (position > overshoot)
			overshoot = position;
		if (done)
			break;
	}

	if (out_velocity)
		*out_velocity = velocity;
	if (out_overshoot)
		*out_overshoot = overshoot;

	return position;
}

static void test_registry(void) {
	CHECK(spring_exists("default"));
	CHECK(spring_exists("bouncy"));
	CHECK(!spring_exists("nope"));
	CHECK(!spring_exists(NULL));
	CHECK(spring_find("gentle") != NULL);
}

static void test_settles(void) {
	spring_curve_t *curve = spring_find("default");
	CHECK(curve != NULL);
	if (!curve)
		return;

	double velocity = 0.0;
	double overshoot = 0.0;
	double position = simulate(curve, &velocity, &overshoot);

	CHECK_NEAR(position, 1.0, 0.01);
	CHECK_NEAR(velocity, 0.0, 0.05);
	CHECK(overshoot > 1.0);
	CHECK(overshoot < 1.2);

	bool done = false;
	spring_evaluate(curve, 1.0 / 120.0, &position, &velocity, &done);
	CHECK(done);
}

static void test_overshoot(void) {
	spring_curve_t *curve = spring_find("bouncy");
	CHECK(curve != NULL);
	if (!curve)
		return;

	double overshoot = 0.0;
	simulate(curve, NULL, &overshoot);
	CHECK(overshoot > 1.0);
}

static void test_zero_dt(void) {
	spring_curve_t *curve = spring_find("default");
	if (!curve)
		return;

	double position = 0.0;
	double velocity = 0.0;
	bool done = true;

	double result = spring_evaluate(curve, 0.0, &position, &velocity, &done);
	CHECK_NEAR(result, 0.0, 1e-9);
	CHECK_NEAR(position, 0.0, 1e-9);
	CHECK(velocity == 0.0);
	CHECK(!done);
}

static void test_invalid(void) {
	double position = 0.5;
	double velocity = 0.0;

	CHECK_NEAR(spring_evaluate(NULL, 1.0 / 60.0, &position, &velocity, NULL), 0.5, 1e-9);
	CHECK_NEAR(position, 0.5, 1e-9);
}

static void test_update(void) {
	spring_curve_t *first = spring_add("unit-test-spring", 200.0, 30.0, 1.0, 0.001, 0.001);
	CHECK(first != NULL);
	if (!first)
		return;

	spring_curve_t *again = spring_add("unit-test-spring", 300.0, 20.0, 2.0, 0.01, 0.01);
	CHECK(again == first);
	CHECK_NEAR(first->stiffness, 300.0, 1e-9);
	CHECK_NEAR(first->mass, 2.0, 1e-9);

	CHECK(spring_add("", 1.0, 1.0, 1.0, 0.001, 0.001) == NULL);
	CHECK(spring_add(NULL, 1.0, 1.0, 1.0, 0.001, 0.001) == NULL);

	// a zero spring constant must not divide by zero
	spring_curve_t *broken = spring_add("unit-test-broken", 0.0, 0.0, 0.0, 0.001, 0.001);
	if (broken) {
		double position = 0.25;
		double velocity = 0.0;
		CHECK_NEAR(spring_evaluate(broken, 1.0 / 60.0, &position, &velocity, NULL), 0.25, 1e-9);
	}
}

static void run_tests(void) {
	spring_init();
	test_registry();
	test_settles();
	test_overshoot();
	test_zero_dt();
	test_invalid();
	test_update();
}

TEST_MAIN(run_tests)
