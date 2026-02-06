#include "../src/ir.h"
#include "../src/jit.h"
#include "../src/liric.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    *out_len = n;
    return buf;
}

static char *read_stdin(size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (!feof(stdin)) {
        if (len + 1024 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) return NULL;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, stdin);
        len += n;
        if (n == 0) break;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

int main(int argc, char **argv) {
    bool jit_mode = false;
    bool dump_ir = false;
    const char *input_file = NULL;
    const char *func_name = "main";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--jit") == 0) jit_mode = true;
        else if (strcmp(argv[i], "--dump-ir") == 0) dump_ir = true;
        else if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) func_name = argv[++i];
        else if (strcmp(argv[i], "-") == 0) input_file = NULL;
        else if (argv[i][0] != '-') input_file = argv[i];
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    size_t src_len;
    char *src;
    if (input_file)
        src = read_file(input_file, &src_len);
    else
        src = read_stdin(&src_len);
    if (!src) {
        fprintf(stderr, "failed to read input\n");
        return 1;
    }

    char err[512] = {0};
    lr_module_t *m = lr_parse_ll(src, src_len, err, sizeof(err));
    if (!m) {
        fprintf(stderr, "parse error: %s\n", err);
        free(src);
        return 1;
    }

    if (dump_ir) {
        lr_module_dump(m, stdout);
        lr_module_free(m);
        free(src);
        return 0;
    }

    if (jit_mode) {
        lr_jit_t *jit = lr_jit_create();
        if (!jit) {
            fprintf(stderr, "failed to create JIT\n");
            lr_module_free(m);
            free(src);
            return 1;
        }

        int rc = lr_jit_add_module(jit, m);
        if (rc != 0) {
            fprintf(stderr, "JIT compilation failed\n");
            lr_jit_destroy(jit);
            lr_module_free(m);
            free(src);
            return 1;
        }

        typedef int (*fn_t)(void);
        fn_t fn; LR_JIT_GET_FN(fn, jit, func_name);
        if (!fn) {
            fprintf(stderr, "function '%s' not found\n", func_name);
            lr_jit_destroy(jit);
            lr_module_free(m);
            free(src);
            return 1;
        }

        int result = fn();
        printf("%d\n", result);

        lr_jit_destroy(jit);
    }

    lr_module_free(m);
    free(src);
    return 0;
}
