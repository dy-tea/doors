#include "config_internal.h"
#include "str.h"
#include "test.h"
#include <stddef.h>

static bool type_is_known(cfg_type_t type) {
	switch (type) {
	case CFG_BOOL:
	case CFG_INT:
	case CFG_FLOAT:
	case CFG_DOUBLE:
	case CFG_ENUM:
	case CFG_STR:
	case CFG_RGBA:
		return true;
	}
	return false;
}

static size_t enum_entry_count(const str_pair_t *values) {
	size_t n = 0;
	while (values[n].name) {
		if (++n > 64)
			// guard against an unterminated table
			return 64;
	}
	return n;
}

static void test_count(void) {
	size_t count = sizeof(settings_table) / sizeof(settings_table[0]);
	CHECK(count == CFG_SETTINGS_COUNT);
	CHECK(count > 0);
}

static void test_entries(void) {
	for (size_t i = 0; i < CFG_SETTINGS_COUNT; i++) {
		const cfg_setting_t *s = &settings_table[i];

		CHECK(s->name != NULL);
		if (!s->name)
			continue;

		CHECK(s->name[0] != '\0');
		CHECK(s->ptr != NULL);
		CHECK(type_is_known(s->type));

		if (s->type == CFG_INT || s->type == CFG_FLOAT || s->type == CFG_DOUBLE)
			CHECK(s->fmt != NULL);

		if (s->type == CFG_STR)
			CHECK(s->size > 0);

		if (s->type == CFG_ENUM) {
			CHECK(s->values != NULL);
			if (!s->values)
				continue;

			size_t entries = enum_entry_count(s->values);
			CHECK(entries > 0);
			CHECK(entries < 64); // a guard hit means the table is unterminated

			for (size_t e = 0; e < entries; e++) {
				CHECK(s->values[e].name[0] != '\0');

				// no duplicate names inside one enum table
				for (size_t other = e + 1; other < entries; other++)
					CHECK(!streqi(s->values[e].name, s->values[other].name));

				// every value is printable
				const char *text = ipc_enum_name(s->values, s->values[e].value);
				CHECK(text != NULL);
			}
		}
	}
}

static void test_unique_names(void) {
	for (size_t i = 0; i < CFG_SETTINGS_COUNT; i++) {
		const cfg_setting_t *s = &settings_table[i];

		if (s->alias) {
			CHECK(s->alias[0] != '\0');
			CHECK(!streq(s->name, s->alias));
		}

		for (size_t j = i + 1; j < CFG_SETTINGS_COUNT; j++) {
			const cfg_setting_t *other = &settings_table[j];

			CHECK(!streq(s->name, other->name));
			if (other->alias)
				CHECK(!streq(s->name, other->alias));
			if (s->alias)
				CHECK(!streq(s->alias, other->name));
			if (s->alias && other->alias)
				CHECK(!streq(s->alias, other->alias));
		}
	}
}

static void test_format(void) {
	for (size_t i = 0; i < CFG_SETTINGS_COUNT; i++) {
		char buf[256] = {0};
		cfg_format_value(&settings_table[i], buf, sizeof(buf));

		CHECK(buf[0] != '\0');
		CHECK(strlen(buf) < sizeof(buf) - 1);
	}
}

static void run_tests(void) {
	test_count();
	test_entries();
	test_unique_names();
	test_format();
}

TEST_MAIN(run_tests)
