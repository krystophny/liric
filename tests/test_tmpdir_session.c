#include <liric/liric_session.h>
#include "platform/platform_os.h"

#include <errno.h>
#include <stdio.h>
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

/* The test is also a linker wrapper: observe the object actually handed to
   the compiler, then invoke the independently configured C compiler. */
static int link_command(int argc, char **argv) {
    const char *record = getenv("LIRIC_TEST_OBJECT_RECORD");
    struct stat st;
    FILE *out;
    CHECK(argc >= 5 && record, "linker wrapper arguments");
    CHECK(stat(argv[3], &st) == 0 && S_ISREG(st.st_mode),
          "emitted temporary object exists at linker invocation");
    out = fopen(record, "w");
    CHECK(out != NULL, "open object-path record");
    fprintf(out, "%s\n", argv[3]);
    CHECK(fclose(out) == 0, "close object-path record");
    if (getenv("LIRIC_TEST_LINK_FAIL"))
        return 73;
    argv[0] = LIRIC_TEST_C_COMPILER;
    execv(LIRIC_TEST_C_COMPILER, argv);
    return 1;
}

static int check_object_removed(const char *record, const char *dir) {
    char path[8192];
    FILE *in = fopen(record, "r");
    CHECK(in != NULL, "read observed object path");
    CHECK(fgets(path, sizeof(path), in) != NULL, "object path was recorded");
    fclose(in);
    path[strcspn(path, "\n")] = '\0';
    CHECK(strncmp(path, dir, strlen(dir)) == 0 &&
          path[strlen(dir)] == '/', "actual linker object is inside TMPDIR");
    CHECK(access(path, F_OK) != 0 && errno == ENOENT,
          "temporary object is removed after emission");
    return 0;
}

int main(int argc, char **argv) {
    char dir[4096], source[8192], object[8192], executable[8192];
    char record[8192], missing[8192], trailing[8192];
    char *self, *temporary = NULL;
    const char *extras[1];
    lr_session_config_t cfg = {.mode = LR_MODE_IR, .backend = LR_SESSION_BACKEND_ISEL};
    lr_error_t err = {0};
    lr_session_t *session;
    lr_type_t *i32;
    struct stat st;
    FILE *out;
    int fd, status;

    if (argc > 1 && strcmp(argv[1], "-o") == 0)
        return link_command(argc, argv);
    CHECK(argc == 2, "scratch root argument");
    CHECK(snprintf(dir, sizeof(dir), "%s/tmpdir session XXXXXX", argv[1]) <
          (int)sizeof(dir), "scratch root fits");
    CHECK(mkdtemp(dir) != NULL, "create scratch directory containing spaces");
    snprintf(source, sizeof(source), "%s/extra source.c", dir);
    snprintf(object, sizeof(object), "%s/extra object.o", dir);
    snprintf(executable, sizeof(executable), "%s/emitted program", dir);
    snprintf(record, sizeof(record), "%s/object path", dir);
    snprintf(missing, sizeof(missing), "%s/missing", dir);
    snprintf(trailing, sizeof(trailing), "%s/", dir);
    CHECK(setenv("TMPDIR", trailing, 1) == 0, "set TMPDIR with trailing slash");
    fd = lr_platform_mkstemp("private_", &temporary);
    CHECK(fd >= 0 && temporary, "create private temporary");
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0600,
          "temporary has private permissions");
    CHECK(strncmp(temporary, trailing, strlen(trailing)) == 0,
          "temporary uses requested directory");
    close(fd);
    unlink(temporary);
    free(temporary);

    out = fopen(source, "w");
    CHECK(out != NULL, "create independent extra object source");
    fputs("int extra_value(void) { return 7; }\n", out);
    CHECK(fclose(out) == 0, "close extra object source");
    {
        char *const command[] = {LIRIC_TEST_C_COMPILER, "-c", source,
                                 "-o", object, NULL};
        CHECK(lr_platform_run_process(command, false, &status) == 0 && status == 0,
              "independent compiler builds extra object");
    }
    self = realpath(argv[0], NULL);
    CHECK(self != NULL && setenv("CC", self, 1) == 0, "select observing linker");
    free(self);
    CHECK(setenv("LIRIC_TEST_OBJECT_RECORD", record, 1) == 0, "select path record");
    session = lr_session_create(&cfg, &err);
    CHECK(session != NULL, "create session");
    i32 = lr_type_i32_s(session);
    CHECK(lr_session_func_begin(session, "main", i32, NULL, 0, false, &err) == 0,
          "begin main");
    CHECK(lr_session_set_block(session, lr_session_block(session), &err) == 0,
          "select main block");
    lr_emit_ret(session, LR_IMM(42, i32));
    CHECK(lr_session_func_end_preserve_ir(session, &err) == 0, "end main");
    extras[0] = object;
    CHECK(lr_session_emit_exe_objects(session, executable, extras, 1, &err) == 0,
          err.msg);
    CHECK(check_object_removed(record, dir) == 0, "successful-link cleanup");
    {
        char *const command[] = {executable, NULL};
        CHECK(lr_platform_run_process(command, false, &status) == 0 && status == 42,
              "emitted executable returns independently expected 42");
    }
    unlink(executable);
    CHECK(setenv("LIRIC_TEST_LINK_FAIL", "1", 1) == 0, "force linker failure");
    CHECK(lr_session_emit_exe_objects(session, executable, extras, 1, &err) != 0 &&
          strstr(err.msg, "73"), "linker failure is reported");
    CHECK(check_object_removed(record, dir) == 0, "failed-link cleanup");
    unsetenv("LIRIC_TEST_LINK_FAIL");
    CHECK(setenv("TMPDIR", missing, 1) == 0, "select nonexistent TMPDIR");
    CHECK(lr_session_emit_exe_objects(session, executable, extras, 1, &err) != 0 &&
          strstr(err.msg, "temporary"), "invalid TMPDIR fails without fallback");
    CHECK(access(executable, F_OK) != 0, "failed emission creates no executable");
    lr_session_destroy(session);
    unlink(source);
    unlink(object);
    unlink(record);
    CHECK(rmdir(dir) == 0, "all generated temporary files are cleaned up");
    puts("TMPDIR executable emission and cleanup: ok");
    return 0;
}
