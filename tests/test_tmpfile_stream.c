#include "platform/platform_os.h"
#include <liric/liric_compat.h>
#include <liric/liric_legacy.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        return 1; \
    } \
} while (0)

static int check_compat_dump(const char *dir, const char *missing) {
    lc_context_t *context = lc_context_create();
    lc_module_compat_t *module;
    lc_value_t *function_value, *block_value, *value;
    lr_type_t *i32, *function_type;
    lr_func_t *function;
    char *text;
    size_t length = 0;

    CHECK(context != NULL, "create compatibility context");
    module = lc_module_create(context, "scratch_module");
    CHECK(module != NULL, "create compatibility module");
    i32 = lc_get_int_type(module, 32);
    function_type = lr_type_func_new(lc_module_get_ir(module), i32, NULL, 0, false);
    function_value = lc_func_create(module, "scratch_ret", function_type);
    CHECK(function_value != NULL, "create compatibility function");
    function = lc_value_get_func(function_value);
    block_value = lc_block_create(module, function, "entry");
    CHECK(block_value != NULL, "create compatibility block");
    value = lc_value_const_int(module, i32, 42, 32);
    lc_create_ret(module, lc_value_get_block(block_value), value);
    CHECK(setenv("TMPDIR", dir, 1) == 0, "select compatibility scratch directory");
    text = lc_module_sprint(module, &length);
    CHECK(text != NULL && length == strlen(text), "serialize complete module text");
    CHECK(strstr(text, "scratch_ret") && strstr(text, "ret i32 42"),
          "serialized function preserves independently expected return");
    free(text);
    CHECK(setenv("TMPDIR", missing, 1) == 0, "select missing compatibility directory");
    CHECK(lc_module_sprint(module, &length) == NULL && errno == ENOENT,
          "compatibility serialization reports missing scratch directory");
    lc_module_destroy(module);
    lc_context_destroy(context);
    return 0;
}

int main(int argc, char **argv) {
    const unsigned char expected[] = {0, 1, 0xff, 3, 0, 5};
    unsigned char actual[sizeof(expected)];
    char dir[4096], missing[8192];
    struct stat st;
    FILE *stream;
    int fd;

    CHECK(argc == 2, "scratch root argument");
    CHECK(snprintf(dir, sizeof(dir), "%s/scratch stream XXXXXX", argv[1]) <
          (int)sizeof(dir), "scratch root fits");
    CHECK(mkdtemp(dir) != NULL, "create directory with spaces");
    CHECK(setenv("TMPDIR", dir, 1) == 0, "select scratch directory");
    stream = lr_platform_tmpfile();
    CHECK(stream != NULL, "create scratch stream");
    fd = fileno(stream);
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode), "stream is a regular file");
    CHECK((st.st_mode & 0777) == 0600, "scratch stream is private");
    CHECK(st.st_nlink == 0, "scratch stream has no directory entry");
    CHECK(fwrite(expected, 1, sizeof(expected), stream) == sizeof(expected),
          "write bytes including NUL and high bit");
    CHECK(ftell(stream) == (long)sizeof(expected), "stream position after write");
    CHECK(fseek(stream, 2, SEEK_SET) == 0 && fgetc(stream) == 0xff,
          "seek reads expected byte");
    rewind(stream);
    CHECK(fread(actual, 1, sizeof(actual), stream) == sizeof(actual) &&
          memcmp(actual, expected, sizeof(actual)) == 0, "binary roundtrip");
    CHECK(fclose(stream) == 0, "close scratch stream");
    CHECK(fstat(fd, &st) != 0 && errno == EBADF, "close releases descriptor");
    snprintf(missing, sizeof(missing), "%s/missing", dir);
    CHECK(check_compat_dump(dir, missing) == 0, "compatibility scratch consumer");
    CHECK(rmdir(dir) == 0, "streams leave no filesystem entries");
    CHECK(setenv("TMPDIR", missing, 1) == 0, "select nonexistent scratch directory");
    CHECK(lr_platform_tmpfile() == NULL && errno == ENOENT,
          "invalid TMPDIR fails with original errno and no fallback");
    puts("TMPDIR binary scratch stream and cleanup: ok");
    return 0;
}
