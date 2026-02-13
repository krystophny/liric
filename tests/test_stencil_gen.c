#include "stencil_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
static int has_hole(const lr_stencil_t *st, lr_stencil_hole_t hole) {
    uint8_t i;
    if (!st) {
        return 0;
    }
    for (i = 0; i < st->n_relocs; i++) {
        if (st->relocs[i].hole == hole) {
            return 1;
        }
    }
    return 0;
}
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64)) && \
    defined(LIRIC_STENCIL_GEN_EXE) && defined(LIRIC_STENCIL_SOURCE_DIR) && \
    defined(LIRIC_STENCIL_CC)
static int compare_files_equal(const char *a_path, const char *b_path) {
    FILE *a = fopen(a_path, "rb");
    FILE *b = fopen(b_path, "rb");
    int eq = 1;
    int ca, cb;
    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return 0;
    }
    do {
        ca = fgetc(a);
        cb = fgetc(b);
        if (ca != cb) {
            eq = 0;
            break;
        }
    } while (ca != EOF && cb != EOF);
    fclose(a);
    fclose(b);
    return eq;
}
#endif

int test_stencil_generated_lookup_core_entries(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    const lr_stencil_t *add_i32 = lr_stencil_lookup_generated("add_i32");
    const lr_stencil_t *sub_i64 = lr_stencil_lookup_generated("sub_i64");
    const lr_stencil_t *fadd_f64 = lr_stencil_lookup_generated("fadd_f64");

    TEST_ASSERT(lr_stencil_count_generated() >= 3, "generated stencil count");
    TEST_ASSERT(add_i32 != NULL, "add_i32 generated stencil exists");
    TEST_ASSERT(sub_i64 != NULL, "sub_i64 generated stencil exists");
    TEST_ASSERT(fadd_f64 != NULL, "fadd_f64 generated stencil exists");

    TEST_ASSERT(add_i32->size > 0, "add_i32 has machine code");
    TEST_ASSERT(add_i32->n_relocs >= 3, "add_i32 has relocations");
    TEST_ASSERT(has_hole(add_i32, LR_STENCIL_HOLE_SRC0_OFF), "add_i32 src0 hole");
    TEST_ASSERT(has_hole(add_i32, LR_STENCIL_HOLE_SRC1_OFF), "add_i32 src1 hole");
    TEST_ASSERT(has_hole(add_i32, LR_STENCIL_HOLE_DST_OFF), "add_i32 dst hole");
#else
    TEST_ASSERT(lr_stencil_count_generated() == 0, "no generated stencils on this platform");
#endif
    return 0;
}

int test_stencil_generated_lookup_unknown_returns_null(void) {
    TEST_ASSERT(lr_stencil_lookup_generated("does_not_exist") == NULL,
                "unknown stencil returns null");
    return 0;
}

int test_stencil_gen_deterministic_output(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64)) && \
    defined(LIRIC_STENCIL_GEN_EXE) && defined(LIRIC_STENCIL_SOURCE_DIR) && \
    defined(LIRIC_STENCIL_CC)
    char out1[256];
    char out2[256];
    char cmd[2048];
    int rc;
    long pid = (long)getpid();

    snprintf(out1, sizeof(out1), "/tmp/liric_stencil_gen_%ld_a.h", pid);
    snprintf(out2, sizeof(out2), "/tmp/liric_stencil_gen_%ld_b.h", pid);

    snprintf(cmd, sizeof(cmd),
             "\"%s\" --compiler \"%s\" --input-dir \"%s\" --output \"%s\"",
             LIRIC_STENCIL_GEN_EXE, LIRIC_STENCIL_CC, LIRIC_STENCIL_SOURCE_DIR, out1);
    rc = system(cmd);
    TEST_ASSERT(rc == 0, "first stencil_gen run");

    snprintf(cmd, sizeof(cmd),
             "\"%s\" --compiler \"%s\" --input-dir \"%s\" --output \"%s\"",
             LIRIC_STENCIL_GEN_EXE, LIRIC_STENCIL_CC, LIRIC_STENCIL_SOURCE_DIR, out2);
    rc = system(cmd);
    TEST_ASSERT(rc == 0, "second stencil_gen run");

    TEST_ASSERT(compare_files_equal(out1, out2), "generated headers are deterministic");
    unlink(out1);
    unlink(out2);
#endif
    return 0;
}

int test_stencil_gen_missing_input_fails(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64)) && \
    defined(LIRIC_STENCIL_GEN_EXE) && defined(LIRIC_STENCIL_CC)
    char out[256];
    char cmd[2048];
    int rc;
    long pid = (long)getpid();

    snprintf(out, sizeof(out), "/tmp/liric_stencil_gen_%ld_fail.h", pid);
    snprintf(cmd, sizeof(cmd),
             "\"%s\" --compiler \"%s\" --input-dir \"/tmp/liric_no_such_dir_%ld\" --output \"%s\"",
             LIRIC_STENCIL_GEN_EXE, LIRIC_STENCIL_CC, pid, out);
    rc = system(cmd);
    TEST_ASSERT(rc != 0, "stencil_gen should fail for missing input directory");
    unlink(out);
#endif
    return 0;
}
