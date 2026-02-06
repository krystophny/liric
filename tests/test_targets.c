#include "../src/jit.h"
#include "../src/target.h"
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

int test_host_target_name(void) {
    const char *name = lr_jit_host_target_name();
    TEST_ASSERT(name != NULL, "host target name exists");
    TEST_ASSERT(name[0] != '\0', "host target name non-empty");
    TEST_ASSERT(strcmp(name, "x86_64") == 0 || strcmp(name, "aarch64") == 0,
                "host target is known");
    return 0;
}

int test_create_host_target(void) {
    const char *name = lr_jit_host_target_name();
    lr_jit_t *jit = lr_jit_create_for_target(name);
    TEST_ASSERT(jit != NULL, "create host target jit");

    const char *selected = lr_jit_target_name(jit);
    TEST_ASSERT(selected != NULL, "jit target name exists");
    TEST_ASSERT(strcmp(selected, name) == 0, "jit uses requested host target");

    lr_jit_destroy(jit);
    return 0;
}

int test_create_unknown_target_fails(void) {
    lr_jit_t *jit = lr_jit_create_for_target("unknown-target");
    TEST_ASSERT(jit == NULL, "unknown target rejected");
    return 0;
}

int test_non_host_target_fails(void) {
    const char *host = lr_jit_host_target_name();
    const char *other = strcmp(host, "x86_64") == 0 ? "aarch64" : "x86_64";

    lr_jit_t *jit = lr_jit_create_for_target(other);
    TEST_ASSERT(jit == NULL, "non-host target rejected");
    return 0;
}

int test_load_missing_runtime_library_fails(void) {
    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");
    int rc = lr_jit_load_library(jit, "/definitely/not/a/real/library/path.so");
    TEST_ASSERT(rc != 0, "missing library rejected");
    lr_jit_destroy(jit);
    return 0;
}
