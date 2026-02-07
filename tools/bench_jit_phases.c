// Fine-grained JIT phase profiling — measures each sub-phase of lr_jit_add_module.
// Links directly against liric internals (not public API).
//
// Build: cmake --build build && the binary is built automatically

#include "../src/ir.h"
#include "../src/ll_parser.h"
#include "../src/jit.h"
#include "../src/target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>
static double now_us(void) {
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0) mach_timebase_info(&info);
    uint64_t t = mach_absolute_time();
    return (double)(t * info.numer / info.denom) / 1e3;
}
#else
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
#endif

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
    const char *input_file = NULL;
    const char *load_libs[64];
    int num_load_libs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (num_load_libs < 64) load_libs[num_load_libs++] = argv[++i];
        }
        else if (argv[i][0] != '-')
            input_file = argv[i];
        else {
            fprintf(stderr, "usage: bench_jit_phases [--iters N] [--load-lib LIB] file.ll\n");
            return 1;
        }
    }
    if (!input_file) {
        fprintf(stderr, "usage: bench_jit_phases [--iters N] [--load-lib LIB] file.ll\n");
        return 1;
    }

    size_t src_len;
    char *src = read_file(input_file, &src_len);
    if (!src) { fprintf(stderr, "failed to read %s\n", input_file); return 1; }

    double t_writable = 0, t_resolve = 0;
    double t_isel = 0, t_encode = 0, t_executable = 0;
    uint32_t total_funcs = 0, total_globals = 0, total_ir_insts = 0;
    uint32_t total_mir_insts = 0;

    for (int iter = 0; iter < iters; iter++) {
        char err[512] = {0};
        lr_arena_t *arena = lr_arena_create(0);
        lr_module_t *m = lr_parse_ll_text(src, src_len, arena, err, sizeof(err));
        if (!m) {
            fprintf(stderr, "parse error: %s\n", err);
            lr_arena_destroy(arena);
            free(src);
            return 1;
        }

        lr_jit_t *jit = lr_jit_create();
        if (!jit) { lr_arena_destroy(arena); free(src); return 1; }

        for (int li = 0; li < num_load_libs; li++) {
            if (lr_jit_load_library(jit, load_libs[li]) != 0) {
                fprintf(stderr, "failed to load: %s\n", load_libs[li]);
                lr_jit_destroy(jit); lr_arena_destroy(arena); free(src);
                return 1;
            }
        }

        // --- Phase 1: make_writable ---
        double p0 = now_us();
        // Access jit internals: make_writable is static in jit.c
        // We'll measure the full lr_jit_add_module but with instrumented internals
        // Instead, let's measure by reconstructing the pipeline:
#if defined(__APPLE__) && defined(__aarch64__)
        pthread_jit_write_protect_np(0);
#endif
        double p1 = now_us();
        t_writable += (p1 - p0);

        // --- Phase 2: materialize globals ---
        // Count globals
        if (iter == 0) {
            for (lr_global_t *g = m->first_global; g; g = g->next)
                total_globals++;
        }
        // (globals are materialized inside lr_jit_add_module, we can't split further
        //  without duplicating internal code, so we time the whole add_module below)

        // Just time the entire lr_jit_add_module as one unit for now,
        // then also time a version that skips globals:
        double p2 = now_us();
        int rc = lr_jit_add_module(jit, m);
        double p3 = now_us();

        if (rc != 0) {
            fprintf(stderr, "JIT failed\n");
            lr_jit_destroy(jit); lr_arena_destroy(arena);
            continue;
        }

        // Count IR instructions and functions
        if (iter == 0) {
            for (lr_func_t *f = m->first_func; f; f = f->next) {
                if (f->is_decl) continue;
                total_funcs++;
                for (lr_block_t *b = f->first_block; b; b = b->next)
                    for (lr_inst_t *inst = b->first; inst; inst = inst->next)
                        total_ir_insts++;
            }
        }

        // For a more detailed breakdown, let's also time isel+encode separately
        // by doing a second compilation pass (parse again, compile manually)
        // --- Detailed phase timing (separate isel/encode) ---
        lr_arena_t *arena2 = lr_arena_create(0);
        lr_module_t *m2 = lr_parse_ll_text(src, src_len, arena2, err, sizeof(err));
        if (!m2) { lr_arena_destroy(arena2); lr_jit_destroy(jit); lr_arena_destroy(arena); continue; }

        // Pre-resolve globals using the already-populated JIT symbol table
        // (we can't easily split resolve vs isel without accessing internals)
        // Instead, time isel+encode together per function
        const lr_target_t *target = jit->target;
        for (lr_func_t *f = m2->first_func; f; f = f->next) {
            if (f->is_decl) continue;

            // Resolve global operands (convert LR_VAL_GLOBAL to LR_VAL_IMM_I64)
            double r0 = now_us();
            for (lr_block_t *b = f->first_block; b; b = b->next) {
                for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
                    for (uint32_t i = 0; i < inst->num_operands; i++) {
                        lr_operand_t *op = &inst->operands[i];
                        if (op->kind != LR_VAL_GLOBAL) continue;
                        const char *name = lr_module_symbol_name(m2, op->global_id);
                        if (!name) continue;
                        void *addr = lr_jit_get_function(jit, name);
                        if (!addr) addr = dlsym(RTLD_DEFAULT, name);
                        if (addr) {
                            op->kind = LR_VAL_IMM_I64;
                            op->imm_i64 = (int64_t)(intptr_t)addr;
                        }
                    }
                }
            }
            double r1 = now_us();
            t_resolve += (r1 - r0);

            // ISel
            lr_mfunc_t *mf = lr_arena_new(arena2, lr_mfunc_t);
            mf->arena = arena2;
            double i0 = now_us();
            target->isel_func(f, mf, m2);
            double i1 = now_us();
            t_isel += (i1 - i0);

            // Count MIR instructions
            if (iter == 0) {
                for (lr_mblock_t *mb = mf->first_block; mb; mb = mb->next)
                    for (lr_minst_t *mi = mb->first; mi; mi = mi->next)
                        total_mir_insts++;
            }

            // Encode
            uint8_t tmp_buf[65536];
            size_t code_len = 0;
            double e0 = now_us();
            target->encode_func(mf, tmp_buf, sizeof(tmp_buf), &code_len);
            double e1 = now_us();
            t_encode += (e1 - e0);
        }

        // make_executable
        double x0 = now_us();
#if defined(__APPLE__) && defined(__aarch64__)
        pthread_jit_write_protect_np(1);
#endif
        __builtin___clear_cache((char *)jit->code_buf,
                                (char *)(jit->code_buf + jit->code_size));
        double x1 = now_us();
        t_executable += (x1 - x0);

        // subtract extra make_executable from total (we did it twice)
        double total_jit = (p3 - p2);
        (void)total_jit;

        lr_arena_destroy(arena2);
        lr_jit_destroy(jit);
        lr_arena_destroy(arena);
    }

    double d = (double)iters;
    printf("file:          %s\n", input_file);
    printf("ll_bytes:      %zu\n", src_len);
    printf("functions:     %u\n", total_funcs);
    printf("globals:       %u\n", total_globals);
    printf("ir_insts:      %u\n", total_ir_insts);
    printf("mir_insts:     %u\n", total_mir_insts);
    printf("iters:         %d\n", iters);
    printf("\n--- Average per iteration (microseconds) ---\n");
    printf("make_writable:  %7.2f us\n", t_writable / d);
    printf("resolve_syms:   %7.2f us\n", t_resolve / d);
    printf("isel:           %7.2f us\n", t_isel / d);
    printf("encode:         %7.2f us\n", t_encode / d);
    printf("make_executable:%7.2f us\n", t_executable / d);
    printf("isel+encode:    %7.2f us\n", (t_isel + t_encode) / d);
    printf("resolve+isel+en:%7.2f us\n", (t_resolve + t_isel + t_encode) / d);

    free(src);
    return 0;
}
