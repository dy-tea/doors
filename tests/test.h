#pragma once

#include <stdio.h>
#include <string.h>

static int test_failures;
static int test_checks;

#define CHECK(cond) \
	do { \
		test_checks++; \
		if (!(cond)) { \
			test_failures++; \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CHECK_STREQ(actual, expected) \
	do { \
		test_checks++; \
		const char *_a = (actual), *_e = (expected); \
		if (!_a || !_e || strcmp(_a, _e) != 0) { \
			test_failures++; \
			fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, \
				_e ? _e : "(null)", _a ? _a : "(null)"); \
		} \
	} while (0)

#define CHECK_NEAR(actual, expected, epsilon) \
	do { \
		test_checks++; \
		double _a = (double)(actual), _e = (double)(expected); \
		double _d = _a > _e ? _a - _e : _e - _a; \
		if (!(_d <= (epsilon))) { \
			test_failures++; \
			fprintf(stderr, "%s:%d: expected %g, got %g\n", __FILE__, __LINE__, _e, _a); \
		} \
	} while (0)

#define TEST_MAIN(name) \
	int main(void) { \
		name(); \
		if (test_failures) { \
			fprintf(stderr, "%d of %d checks failed\n", test_failures, test_checks); \
			return 1; \
		} \
		printf("%d checks passed\n", test_checks); \
		return 0; \
	}
