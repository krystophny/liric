#include "../src/ir.h"
#include "../src/jit.h"
#include "../src/liric.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t nread = 0;

    if (!f) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long flen = ftell(f);
    if (flen < 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = (char *)malloc((size_t)flen + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    nread = fread(buf, 1, (size_t)flen, f);
    buf[nread] = '\0';
    fclose(f);

    *out_len = nread;
    return buf;
}

static int run_symbol(lr_jit_t *jit, const char *func_name, const char *sig) {
    void *sym = lr_jit_get_function(jit, func_name);
    if (!sym) {
        fprintf(stderr, "function '%s' not found\n", func_name);
        return 3;
    }

    if (strcmp(sig, "i32") == 0) {
        int32_t (*fn)(void) = NULL;
        lr_jit_fn_to_ptr(&fn, sym);
        return fn ? (int)(fn() & 0xff) : 3;
    }

    if (strcmp(sig, "i64") == 0) {
        int64_t (*fn)(void) = NULL;
        lr_jit_fn_to_ptr(&fn, sym);
        return fn ? (int)(fn() & 0xff) : 3;
    }

    if (strcmp(sig, "void") == 0) {
        void (*fn)(void) = NULL;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) {
            return 3;
        }
        fn();
        return 0;
    }

    fprintf(stderr, "unsupported signature: %s\n", sig);
    return 2;
}

int main(int argc, char **argv) {
    const char *func_name = "main";
    const char *sig = "i32";
    const char *input_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) {
            func_name = argv[++i];
        } else if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) {
            sig = argv[++i];
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "missing input file\n");
        return 1;
    }

    size_t src_len = 0;
    char *src = read_file(input_file, &src_len);
    if (!src) {
        fprintf(stderr, "failed to read input file\n");
        return 1;
    }

    char err[512] = {0};
    lr_module_t *m = lr_parse_ll(src, src_len, err, sizeof(err));
    if (!m) {
        fprintf(stderr, "parse error: %s\n", err);
        free(src);
        return 1;
    }

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

    int run_rc = run_symbol(jit, func_name, sig);

    lr_jit_destroy(jit);
    lr_module_free(m);
    free(src);
    return run_rc;
}
