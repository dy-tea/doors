#include "str.h"
#include "test.h"

static void test_streq(void) {
	CHECK(streq("", ""));
	CHECK(streq("abc", "abc"));
	CHECK(!streq("abc", "abd"));
	CHECK(!streq(NULL, "abc"));
	CHECK(streq(NULL, NULL));

	CHECK(streqi("ABC", "abc"));
	CHECK(!streqi("abc", "abd"));
}

static void test_copy_append(void) {
	char buf[8];

	CHECK(str_copy(buf, sizeof(buf), "abc") == 3);
	CHECK_STREQ(buf, "abc");

	// truncate
	CHECK(str_copy(buf, 4, "abcdef") == 3);
	CHECK_STREQ(buf, "abc");

	CHECK(str_copy(buf, 4, NULL) == 0);
	CHECK_STREQ(buf, "");

	str_copy(buf, sizeof(buf), "ab");
	CHECK(str_append(buf, sizeof(buf), "cd") == 2);
	CHECK_STREQ(buf, "abcd");

	// no room left
	str_copy(buf, 4, "abc");
	CHECK(str_append(buf, 4, "d") == 0);
	CHECK_STREQ(buf, "abc");
	CHECK(str_append(buf, 0, "d") == 0);
}

static void test_append_sep(void) {
	char buf[8];

	str_copy(buf, sizeof(buf), "");
	CHECK(str_append_sep(buf, sizeof(buf), ",", "a"));
	CHECK_STREQ(buf, "a");

	CHECK(str_append_sep(buf, sizeof(buf), ",", "bc"));
	CHECK_STREQ(buf, "a,bc");

	str_copy(buf, sizeof(buf), "");
	CHECK(!str_append_sep(buf, 4, ",", "abcdef"));
	CHECK_STREQ(buf, "abc");

	str_copy(buf, 4, "abc");
	CHECK(!str_append_sep(buf, 4, ",", "d"));
	CHECK_STREQ(buf, "abc");
}

static void test_prefix_suffix(void) {
	CHECK(str_starts("foobar", "foo"));
	CHECK(!str_starts("foobar", "bar"));
	CHECK(!str_starts(NULL, "bar"));
	CHECK(str_starts("foo", "foo"));

	CHECK_STREQ(str_skip_prefix("foobar", "foo"), "bar");
	CHECK(str_skip_prefix("foobar", "bar") == NULL);

	CHECK(str_ends("foobar", "bar"));
	CHECK(!str_ends("foo", "foobar"));
	CHECK(!str_ends(NULL, "bar"));
	CHECK(str_ends("bar", "bar"));
}

static void test_trim(void) {
	char buf[16];

	str_copy(buf, sizeof(buf), "  hi  ");
	CHECK_STREQ(str_ltrim(buf), "hi  ");
	CHECK_STREQ(str_rtrim(buf), "  hi");
	CHECK_STREQ(str_trim(buf), "hi");

	str_copy(buf, sizeof(buf), "line\r\n");
	str_chomp(buf);
	CHECK_STREQ(buf, "line");

	str_copy(buf, sizeof(buf), "line\n");
	str_chomp(buf);
	CHECK_STREQ(buf, "line");

	// only one trailing newline
	str_copy(buf, sizeof(buf), "line\n\n");
	str_chomp(buf);
	CHECK_STREQ(buf, "line\n");

	str_copy(buf, sizeof(buf), "line");
	str_chomp(buf);
	CHECK_STREQ(buf, "line");
}

static void test_parse_bool(void) {
	bool v = false;

	CHECK(str_parse_bool("true", &v) && v);
	CHECK(str_parse_bool("ON", &v) && v);
	CHECK(str_parse_bool("enabled", &v) && v);

	CHECK(str_parse_bool("false", &v) && !v);
	CHECK(str_parse_bool("NO", &v) && !v);
	CHECK(str_parse_bool("0", &v) && !v);

	CHECK(!str_parse_bool("maybe", &v));
	CHECK(!str_parse_bool("", &v));
	CHECK(!str_parse_bool(NULL, &v));

	CHECK_STREQ(str_bool_text(true), "true");
	CHECK_STREQ(str_bool_text(false), "false");
}

static void test_split(void) {
	char buf[32];
	char *out[8];
	int n;

	str_copy(buf, sizeof(buf), "a,b,,c");
	n = str_split(buf, ",", out, 8);
	CHECK(n == 3);
	if (n == 3) {
		CHECK_STREQ(out[0], "a");
		CHECK_STREQ(out[1], "b");
		CHECK_STREQ(out[2], "c");
	}

	// max caps the number of tokens returned
	str_copy(buf, sizeof(buf), "1 2 3 4");
	n = str_split(buf, " ", out, 2);
	CHECK(n == 2);

	str_copy(buf, sizeof(buf), "   ");
	CHECK(str_split(buf, " \t", out, 8) == 0);
}

static void test_hex(void) {
	CHECK(str_hex_digit('0') == 0);
	CHECK(str_hex_digit('9') == 9);
	CHECK(str_hex_digit('a') == 10);
	CHECK(str_hex_digit('F') == 15);
	CHECK(str_hex_digit('g') == -1);
	CHECK(str_hex_digit('/') == -1);
}

static void test_tables(void) {
	const str_pair_t table[] = {
		{"one", 1},
		{"two", 2},
		{NULL, 0},
	};

	CHECK(str_table_find(table, "one") != NULL);
	CHECK(str_table_find(table, "ONE") != NULL);
	CHECK(str_table_find(table, "three") == NULL);
	CHECK(str_table_find(NULL, "one") == NULL);

	const str_pair_t *found = str_table_find_value(table, 2);
	CHECK(found != NULL && streq(found->name, "two"));
	CHECK(str_table_find_value(table, 3) == NULL);
}

static void run_tests(void) {
	test_streq();
	test_copy_append();
	test_append_sep();
	test_prefix_suffix();
	test_trim();
	test_parse_bool();
	test_split();
	test_hex();
	test_tables();
}

TEST_MAIN(run_tests)
