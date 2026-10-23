#include "bezier.h"
#include "str.h"
#include "test.h"

static void test_builtins(void) {
	CHECK(bezier_exists("default"));
	CHECK(bezier_exists("linear"));
	CHECK(bezier_exists("ease_in_out"));
	CHECK(!bezier_exists("nope"));
	CHECK(!bezier_exists(""));
	CHECK(!bezier_exists(NULL));
}

static void test_linear(void) {
	CHECK_NEAR(bezier_evaluate("linear", 0.0), 0.0, 1e-6);
	CHECK_NEAR(bezier_evaluate("linear", 1.0), 1.0, 1e-6);
	CHECK_NEAR(bezier_evaluate("linear", 0.25), 0.25, 0.01);
	CHECK_NEAR(bezier_evaluate("linear", 0.5), 0.5, 0.01);
	CHECK_NEAR(bezier_evaluate("linear", 0.75), 0.75, 0.01);
}

static void test_clamps(void) {
	// outside [0, 1] the curve is clamped
	CHECK(bezier_evaluate("default", -0.5) == 0.0);
	CHECK(bezier_evaluate("default", 1.5) == 1.0);

	// unknown curve
	CHECK_NEAR(bezier_evaluate("nope", 0.3), 0.3, 1e-9);
	CHECK_NEAR(bezier_evaluate_curve(NULL, 0.3), 0.3, 1e-9);
}

static void test_monotonic(void) {
	double previous = 0.0;
	for (int i = 0; i <= 100; i++) {
		double x = (double)i / 100.0;
		double y = bezier_evaluate("default", x);
		CHECK(y >= previous - 1e-6);
		CHECK(y >= 0.0 && y <= 1.0);
		previous = y;
	}
}

static void test_update(void) {
	bezier_curve_t *first = bezier_add("unit-test-curve", 0.1, 0.2, 0.3, 0.4);
	CHECK(first != NULL);
	if (!first)
		return;

	CHECK(bezier_exists("unit-test-curve"));
	CHECK_NEAR(bezier_evaluate("unit-test-curve", 0.0), 0.0, 1e-6);
	CHECK_NEAR(bezier_evaluate("unit-test-curve", 1.0), 1.0, 1e-6);

	bezier_curve_t *again = bezier_add("unit-test-curve", 0.9, 0.9, 0.9, 0.9);
	CHECK(again == first);
	CHECK(bezier_find("unit-test-curve") == first);

	CHECK(bezier_add("", 0.0, 0.0, 1.0, 1.0) == NULL);
	CHECK(bezier_add(NULL, 0.0, 0.0, 1.0, 1.0) == NULL);
}

static void run_tests(void) {
	bezier_init();
	test_builtins();
	test_linear();
	test_clamps();
	test_monotonic();
	test_update();
}

TEST_MAIN(run_tests)
