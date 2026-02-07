#include "../src/ir.h"
#include "../src/ll_parser.h"
#include "../src/jit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    *out_len = n;
    return buf;
}

int main(int argc, char **argv) {
    int iters = 1;
    int json_output = 0;
    int parse_only = 0;
    const char *input_file = NULL;
    const char *load_libs[64];
    int num_load_libs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (strcmp(argv[i], "--json") == 0)
            json_output = 1;
        else if (strcmp(argv[i], "--parse-only") == 0)
            parse_only = 1;
        else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (num_load_libs < 64)
                load_libs[num_load_libs++] = argv[++i];
        }
        else if (argv[i][0] != '-')
            input_file = argv[i];
        else {
            fprintf(stderr, "usage: bench_parse_vs_jit [--iters N] [--json] "
                            "[--parse-only] [--load-lib LIB] file.ll\n");
            return 1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "usage: bench_parse_vs_jit [--iters N] [--json] "
                        "[--parse-only] [--load-lib LIB] file.ll\n");
        return 1;
    }

    size_t src_len;
    char *src = read_file(input_file, &src_len);
    if (!src) {
        fprintf(stderr, "failed to read %s\n", input_file);
        return 1;
    }

    double parse_total = 0, jit_total = 0;
    int num_funcs = 0;

    for (int iter = 0; iter < iters; iter++) {
        char err[512] = {0};

        double t0 = now_ms();
        lr_arena_t *arena = lr_arena_create(0);
        lr_module_t *m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
        double t1 = now_ms();

        if (!m) {
            if (json_output)
                printf("{\"file\":\"%s\",\"error\":\"parse: %s\"}\n", input_file, err);
            else
                fprintf(stderr, "parse error: %s\n", err);
            lr_arena_destroy(arena);
            free(src);
            return 1;
        }

        if (iter == 0) {
            for (lr_func_t *fn = m->first_func; fn; fn = fn->next)
                num_funcs++;
        }

        parse_total += (t1 - t0);

        if (!parse_only) {
            lr_jit_t *jit = lr_jit_create();
            if (!jit) {
                lr_arena_destroy(arena);
                free(src);
                return 1;
            }

            for (int li = 0; li < num_load_libs; li++) {
                if (lr_jit_load_library(jit, load_libs[li]) != 0) {
                    fprintf(stderr, "failed to load library: %s\n", load_libs[li]);
                    lr_jit_destroy(jit);
                    lr_arena_destroy(arena);
                    free(src);
                    return 1;
                }
            }

            double t2 = now_ms();
            int rc = lr_jit_add_module(jit, m);
            double t3 = now_ms();

            lr_jit_destroy(jit);

            if (rc != 0) {
                if (json_output)
                    printf("{\"file\":\"%s\",\"error\":\"jit\"}\n", input_file);
                else
                    fprintf(stderr, "JIT compilation failed\n");
                lr_arena_destroy(arena);
                free(src);
                return 1;
            }

            jit_total += (t3 - t2);
        }

        lr_arena_destroy(arena);
    }

    double parse_avg = parse_total / iters;
    double jit_avg = parse_only ? 0 : jit_total / iters;
    double total_avg = parse_avg + jit_avg;
    double parse_pct = total_avg > 0 ? 100.0 * parse_avg / total_avg : 100.0;

    if (json_output) {
        printf("{\"file\":\"%s\",\"ll_bytes\":%zu,\"num_funcs\":%d,"
               "\"parse_ms\":%.3f,\"jit_ms\":%.3f,\"total_ms\":%.3f,"
               "\"parse_pct\":%.1f,\"iters\":%d,\"parse_only\":%s}\n",
               input_file, src_len, num_funcs,
               parse_avg, jit_avg, total_avg, parse_pct, iters,
               parse_only ? "true" : "false");
    } else {
        printf("file:      %s\n", input_file);
        printf("ll_bytes:  %zu\n", src_len);
        printf("num_funcs: %d\n", num_funcs);
        printf("parse:     %.3f ms (%.1f%%)\n", parse_avg, parse_pct);
        if (!parse_only)
            printf("jit:       %.3f ms (%.1f%%)\n", jit_avg, 100.0 - parse_pct);
        printf("total:     %.3f ms\n", total_avg);
        printf("iters:     %d\n", iters);
    }

    free(src);
    return 0;
}
