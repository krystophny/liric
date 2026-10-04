#include <liric/liric_legacy.h>
#include "ir.h"
#include "jit.h"
#include "target.h"
#include "objfile.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tls_thread_oracle.h"

static lr_jit_t *active_jit;
static void *lookup_counter(void) {
    return lr_jit_get_symbol(active_jit, "counter");
}

int main(int argc, char **argv) {
    char error[512] = {0};
    if (argc > 1 && strcmp(argv[1], "--exe") == 0 && argc != 4) return 1;
    const char *source_path = argc > 1 &&
        (strcmp(argv[1], "--bitcode") == 0 || strcmp(argv[1], "--exe") == 0 ||
         (strcmp(argv[1], "--object") == 0 && argc == 4))
        ? argv[2] : LIRIC_TLS_FIXTURE;
    FILE *input = fopen(source_path, "rb");
    if (!input) return 1;
    if (fseek(input, 0, SEEK_END) != 0) return 1;
    long length = ftell(input);
    if (length < 0 || fseek(input, 0, SEEK_SET) != 0) return 1;
    char *source = malloc((size_t)length + 1);
    if (!source || fread(source, 1, (size_t)length, input) != (size_t)length) return 1;
    fclose(input);
    source[length] = 0;
    lr_module_t *module = argc > 1 && strcmp(argv[1], "--bitcode") == 0
        ? lr_parse_bc((const uint8_t *)source, (size_t)length, error, sizeof(error))
        : lr_parse_ll(source, (size_t)length, error, sizeof(error));
    free(source);
    if (!module) {
        fprintf(stderr, "TLS parse failed: %s\n", error);
        return 1;
    }
    if (argc == 4 && strcmp(argv[1], "--exe") == 0) {
        FILE *out = fopen(argv[3], "wb");
        if (!out) return 1;
        int result = lr_emit_executable(module, lr_target_host(), out, "main");
        if (fclose(out) != 0) result = -1;
        lr_module_free(module);
        return result != 0;
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "--object") == 0) {
        FILE *out = fopen(argv[argc - 1], "wb");
        if (!out) return 1;
        int result = lr_emit_object(module, lr_target_host(), out);
        if (fclose(out) != 0) result = -1;
        lr_module_free(module);
        return result != 0;
    }
    if (argc == 2 && strcmp(argv[1], "--roundtrip") == 0) {
        for (lr_func_t *f = module->first_func; f; f = f->next)
            if (f->first_block && lr_func_finalize(f, module->arena) != 0) return 1;
        FILE *dump = tmpfile();
        if (!dump) return 1;
        lr_module_dump(module, dump);
        if (fseek(dump, 0, SEEK_END) != 0) return 1;
        long size = ftell(dump);
        if (size < 0 || fseek(dump, 0, SEEK_SET) != 0) return 1;
        char *text = malloc((size_t)size + 1);
        if (!text || fread(text, 1, (size_t)size, dump) != (size_t)size) return 1;
        text[size] = 0;
        fclose(dump);
        lr_module_free(module);
        module = lr_parse_ll(text, (size_t)size, error, sizeof(error));
        free(text);
        if (!module) { fprintf(stderr, "TLS roundtrip parse failed: %s\n", error); return 1; }
    }
    lr_jit_t *jit = lr_jit_create();
    if (!jit || lr_jit_add_module(jit, module) != 0) {
        fprintf(stderr, "TLS native compilation failed\n");
        return 1;
    }
    active_jit = jit;
    get_alias = lookup_counter;
    void *address = lr_jit_get_function(jit, "tls_counter");
    memcpy(&get_counter, &address, sizeof(get_counter));
    address = lr_jit_get_function(jit, "tls_zero");
    memcpy(&get_zero, &address, sizeof(get_zero));
    address = lr_jit_get_function(jit, "tls_bytes");
    memcpy(&get_bytes, &address, sizeof(get_bytes));
    address = lr_jit_get_function(jit, "tls_offset");
    memcpy(&get_offset, &address, sizeof(get_offset));
    if (!get_counter || !get_zero || !get_bytes || !get_offset) return 1;
    int result = check_tls_storage();
    lr_jit_destroy(jit);
    lr_module_free(module);
    return result;
}
