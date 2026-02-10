#include "../src/jit.h"
#include "../src/liric.h"
#include "../src/target.h"
#include "../src/bc_decode.h"
#include <stdint.h>
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

int test_target_alias_arm64_resolves(void) {
    const lr_target_t *canonical = lr_target_by_name("aarch64");
    const lr_target_t *alias = lr_target_by_name("arm64");
    TEST_ASSERT(canonical != NULL, "aarch64 target exists");
    TEST_ASSERT(alias != NULL, "arm64 alias exists");
    TEST_ASSERT(strcmp(canonical->name, alias->name) == 0, "arm64 alias maps to aarch64");
    return 0;
}

int test_parse_auto_selects_ll_frontend(void) {
    const char *src =
        "define i32 @main() {\n"
        "entry:\n"
        "  ret i32 7\n"
        "}\n";
    char err[256] = {0};
    lr_module_t *m = lr_parse_auto((const uint8_t *)src, strlen(src), err, sizeof(err));
    TEST_ASSERT(m != NULL, "auto parser accepts LLVM IR text");
    TEST_ASSERT(m->first_func != NULL, "module has function");
    TEST_ASSERT(strcmp(m->first_func->name, "main") == 0, "parsed function name");
    lr_module_free(m);
    return 0;
}

int test_parse_auto_selects_wasm_frontend(void) {
    uint8_t wasm[] = {
        0x00, 0x61, 0x73, 0x6D,
        0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,
        0x03, 0x02, 0x01, 0x00,
        0x07, 0x05, 0x01, 0x01, 'f', 0x00, 0x00,
        0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2A, 0x0B,
    };
    char err[256] = {0};
    lr_module_t *m = lr_parse_auto(wasm, sizeof(wasm), err, sizeof(err));
    TEST_ASSERT(m != NULL, "auto parser accepts WASM binary");
    TEST_ASSERT(m->first_func != NULL, "module has wasm function");
    TEST_ASSERT(strcmp(m->first_func->name, "f") == 0, "wasm export function name");
    lr_module_free(m);
    return 0;
}

int test_parse_auto_selects_bc_frontend(void) {
    const uint8_t bc_raw[] = {0x42, 0x43, 0xC0, 0xDE, 0x35, 0x14, 0x00, 0x00};
    if (lr_bc_parser_available()) {
        TEST_ASSERT(lr_bc_is_bitcode(bc_raw, sizeof(bc_raw)), "BC magic is detected");
        return 0;
    }

    char err[256] = {0};
    lr_module_t *m = lr_parse_auto(bc_raw, sizeof(bc_raw), err, sizeof(err));
    TEST_ASSERT(m == NULL, "invalid/truncated BC is rejected by BC frontend");
    TEST_ASSERT(strstr(err, "decoder support") != NULL, "error reports decoder support is unavailable");
    return 0;
}

static int fake_puts(const char *s) {
    (void)s;
    return 0;
}

int test_symbol_provider_prefers_jit_table(void) {
    lr_jit_t *jit = lr_jit_create();
    TEST_ASSERT(jit != NULL, "jit create");

    lr_jit_add_symbol(jit, "puts", (void *)(uintptr_t)&fake_puts);
    void *sym = lr_jit_get_function(jit, "puts");
    TEST_ASSERT(sym == (void *)(uintptr_t)&fake_puts, "jit-table provider has precedence");

    lr_jit_destroy(jit);
    return 0;
}
