#include "settings.h"
#include "test.h"
#include "types.h"
#include <string.h>

static void test_valid_values(void) {
	char err[256] = {0};
	int before = settings.border_width;

	CHECK(config_validate_value("border_width", "7", err, sizeof(err)));
	CHECK(err[0] == '\0');
	CHECK(settings.border_width == before);

	bool before_bool = settings.borderless_monocle;
	CHECK(config_validate_value("borderless_monocle", "true", err, sizeof(err)));
	CHECK(settings.borderless_monocle == before_bool);

	focus_on_activate_mode_t before_enum = settings.focus_on_activate;
	CHECK(config_validate_value("focus_on_activate", "smart", err, sizeof(err)));
	CHECK(settings.focus_on_activate == before_enum);
}

static void test_invalid_values(void) {
	char err[256] = {0};
	int before = settings.border_width;

	CHECK(!config_validate_value("border_width", "two", err, sizeof(err)));
	CHECK(err[0] != '\0');
	CHECK(settings.border_width == before);

	err[0] = '\0';
	CHECK(!config_validate_value("border_width", "99999999999999999999", err, sizeof(err)));
	CHECK(err[0] != '\0');
	CHECK(settings.border_width == before);

	err[0] = '\0';
	CHECK(!config_validate_value("borderless_monocle", "maybe", err, sizeof(err)));
	CHECK(strstr(err, "expected true or false") != NULL);

	err[0] = '\0';
	CHECK(!config_validate_value("focus_on_activate", "sometimes", err, sizeof(err)));
	CHECK(strstr(err, "expected value to be one of") != NULL);

	err[0] = '\0';
	CHECK(!config_validate_value("no_such_setting", "1", err, sizeof(err)));
	CHECK(strstr(err, "unknown setting") != NULL);
}

static void run_tests(void) {
	test_valid_values();
	test_invalid_values();
}

TEST_MAIN(run_tests)
