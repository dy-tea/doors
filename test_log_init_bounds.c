// test_log_init_bounds.c
// Build: cc -fsanitize=address,undefined -g test_log_init_bounds.c src/log.c \
//        $(pkg-config --cflags --libs wlroots-0.19) -o test_log_init_bounds
#include <stdio.h>
#include <string.h>

extern int log_init(const char *log_file_path);
extern void log_fini(void);

int main(void) {
    char long_path[3000];
    memset(long_path, 'A', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    // Pre-fix: strcpy(log_dir, log_file_path) overflows the 1024-byte
    // static log_dir buffer here, before fopen() is ever reached.
    // ASan should report a global-buffer-overflow on this call pre-patch,
    // and pass cleanly post-patch (fopen() will just fail on the bogus path).
    int rc = log_init(long_path);
    printf("log_init returned %d (expected -1, no crash)\n", rc);
    log_fini();
    return 0;
}
