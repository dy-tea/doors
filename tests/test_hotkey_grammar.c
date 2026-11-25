#include "config.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

static char *write_config(const char *content) {
	char *path = strdup("/tmp/doors-test-hkrc-XXXXXX");
	if (path == NULL)
		return NULL;

	const int fd = mkstemp(path);
	if (fd < 0) {
		free(path);
		return NULL;
	}

	const size_t len = strlen(content);
	if (write(fd, content, len) != (ssize_t)len) {
		close(fd);
		unlink(path);
		free(path);
		return NULL;
	}

	close(fd);
	return path;
}

static void test_hotkey_spec(void) {
	uint32_t modifiers = 0, keycode = 0, flags = 0;
	xkb_keysym_t keysym = XKB_KEY_NoSymbol;
	bool use_keycode = false;

	CHECK(parse_hotkey_spec("alt + Return", &modifiers, &keysym, &keycode, &use_keycode,
		&flags) == NULL);
	CHECK(modifiers == WLR_MODIFIER_ALT);
	CHECK(keysym == XKB_KEY_Return);
	CHECK(!use_keycode);
	CHECK(flags == 0);

	CHECK(parse_hotkey_spec("alt + shift + q", &modifiers, &keysym, &keycode, &use_keycode,
		&flags) == NULL);
	CHECK(modifiers == (WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT));
	CHECK(keysym == XKB_KEY_q);

	// mouse buttons become synthetic keycodes
	CHECK(parse_hotkey_spec("alt + mouse_left", &modifiers, &keysym, &keycode, &use_keycode,
		&flags) == NULL);
	CHECK(modifiers == WLR_MODIFIER_ALT);
	CHECK(use_keycode);
	CHECK(keycode == mouse_button_to_keycode(0));

	// numeric keys are keycodes too
	CHECK(parse_hotkey_spec("super+1", &modifiers, &keysym, &keycode, &use_keycode, &flags) == NULL);
	CHECK(modifiers == WLR_MODIFIER_LOGO);
	CHECK(use_keycode);
	CHECK(keycode == 2);

	// trailing flag list
	CHECK(parse_hotkey_spec("super + return [release,locked]", &modifiers, &keysym, &keycode,
		&use_keycode, &flags) == NULL);
	CHECK(modifiers == WLR_MODIFIER_LOGO);
	CHECK(keysym == XKB_KEY_Return);
	CHECK(flags == (BINDF_RELEASE | BINDF_LOCKED));

	// catchall binds carry no keysym
	CHECK(parse_hotkey_spec("catchall", &modifiers, &keysym, &keycode, &use_keycode, &flags) == NULL);
	CHECK(flags == BINDF_CATCH_ALL);
	CHECK(keysym == XKB_KEY_NoSymbol);
	CHECK(!use_keycode);

	CHECK_STREQ(parse_hotkey_spec("super + nosuchkey", &modifiers, &keysym, &keycode, &use_keycode,
		&flags), "Unknown keysym");
}

static void test_bind_flags(void) {
	uint32_t flags = 0;
	CHECK(parse_bind_flags("", &flags));
	CHECK(flags == 0);
	CHECK(parse_bind_flags(NULL, &flags));
	CHECK(flags == 0);
	CHECK(parse_bind_flags("release, locked", &flags));
	CHECK(flags == (BINDF_RELEASE | BINDF_LOCKED));
	CHECK(parse_bind_flags("non_consuming", &flags));
	CHECK(flags == BINDF_NON_CONSUMING);
	CHECK(!parse_bind_flags("release, nonsense", &flags));

	char buf[128];
	bind_flags_text(0, buf, sizeof(buf));
	CHECK_STREQ(buf, "");
	bind_flags_text(BINDF_LOCKED | BINDF_REPEAT, buf, sizeof(buf));
	CHECK_STREQ(buf, "locked,repeat");
}

static void test_bind_kind(void) {
	char submap_name[MAXLEN];

	CHECK(parse_bind_kind("foot", submap_name) == BIND_KIND_COMMAND);
	CHECK(submap_name[0] == '\0');

	CHECK(parse_bind_kind("@focus", submap_name) == BIND_KIND_ENTER_SUBMAP);
	CHECK_STREQ(submap_name, "focus");

	CHECK(parse_bind_kind("@exit", submap_name) == BIND_KIND_EXIT_SUBMAP);
}

static void test_modifiers_and_keycodes(void) {
	CHECK(parse_modifiers("") == 0);
	CHECK(parse_modifiers("super") == WLR_MODIFIER_LOGO);
	CHECK(parse_modifiers("mod4 + shift") == (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT));
	CHECK(parse_modifiers("nonsense") == 0);

	CHECK(parse_keysym("Return") == XKB_KEY_Return);
	CHECK(parse_keysym("nosuchkey") == XKB_KEY_NoSymbol);

	CHECK(parse_keycode("mouse_middle") == mouse_button_to_keycode(2));
	CHECK(parse_keycode("5") == 6);
	CHECK(parse_keycode("0") == 11);
	CHECK(parse_keycode("nosuchkey") == 0);
}

static const char *config_text = "# comment line\n"
	"\n"
	"alt + Return\n"
	"\tfoot\n"
	"\n"
	"alt + shift + q -> doorsctl node --close\n"
	"\n"
// brace expansion on both sides
"alt + {1,2}\n"
	"\tdoorsctl desktop {1,2}\n"
	"\n"
	"bell\n"
	"\tffplay bell.oga\n"
	"\n"
	"gesture swipe:3:left\n"
	"\tdoorsctl desktop next\n"
	"\n"
	"hotcorner topleft\n"
	"\tdoorsctl desktop prev\n"
	"\n"
	"@focus\n"
// a submap body is indented, an unindented line ends it
"  escape\n"
	"    @exit\n"
	"  {h,j,k,l}\n"
	"    doorsctl focus {west,south,north,east}\n";

static void test_load_config(void) {
	char *path = write_config(config_text);
	CHECK(path != NULL);
	if (path == NULL)
		return;

	load_hotkeys(path);
	unlink(path);
	free(path);

	CHECK(num_keybinds == 4);
	if (num_keybinds != 4)
		return;

	CHECK(keybinds[0].modifiers == WLR_MODIFIER_ALT);
	CHECK(keybinds[0].keysym == XKB_KEY_Return);
	CHECK_STREQ(keybinds[0].cmd, "foot");
	CHECK(keybinds[0].kind == BIND_KIND_COMMAND);

	CHECK(keybinds[1].modifiers == (WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT));
	CHECK(keybinds[1].keysym == XKB_KEY_q);
	CHECK_STREQ(keybinds[1].cmd, "doorsctl node --close");

	CHECK(keybinds[2].use_keycode);
	CHECK(keybinds[2].keycode == 2);
	CHECK_STREQ(keybinds[2].cmd, "doorsctl desktop 1");
	CHECK(keybinds[3].keycode == 3);
	CHECK_STREQ(keybinds[3].cmd, "doorsctl desktop 2");

	CHECK(bell_bind.kind == BIND_KIND_COMMAND);
	CHECK_STREQ(bell_bind.cmd, "ffplay bell.oga");

	CHECK(num_gesturebinds == 1);
	CHECK(gesture_bindings[0].type == GESTURE_TYPE_SWIPE);
	CHECK(gesture_bindings[0].fingers == 3);
	CHECK((gesture_bindings[0].directions & GESTURE_DIRECTION_LEFT) != 0);
	CHECK_STREQ(gesture_bindings[0].cmd, "doorsctl desktop next");

	CHECK(num_hotcornerbinds == 1);
	CHECK(hotcorner_bindings[0].corner == HOTCORNER_TOPLEFT);
	CHECK_STREQ(hotcorner_bindings[0].cmd, "doorsctl desktop prev");

	submap_t *submap = find_submap_by_name("focus");
	CHECK(submap != NULL);
	if (submap != NULL) {
		CHECK(submap->num_keybinds == 5);
		CHECK(submap->keybinds[0].kind == BIND_KIND_EXIT_SUBMAP);
		CHECK(submap->keybinds[1].keysym == XKB_KEY_h);
		CHECK_STREQ(submap->keybinds[1].cmd, "doorsctl focus west");
		CHECK(submap->keybinds[4].keysym == XKB_KEY_l);
		CHECK_STREQ(submap->keybinds[4].cmd, "doorsctl focus east");
	}

	// loading resets the active submap
	CHECK(active_submap == NULL);
}

static void run_tests(void) {
	test_hotkey_spec();
	test_bind_flags();
	test_bind_kind();
	test_modifiers_and_keycodes();
	test_load_config();
}

TEST_MAIN(run_tests)
