// In-process LLVM JIT compile benchmark — matches bench_parse_vs_jit format.
// Uses LLVM C API to parse .ll text and JIT compile, measuring each phase.
//
// Build:
//   cc -O2 -o build/bench_llvm_jit tools/bench_llvm_jit.c \
//      $(llvm-config --cflags --ldflags --libs core orcjit native irreader) \
//      -lm -lstdc++

#include <llvm-c/Core.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
static double now_ms(void) {
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0) mach_timebase_info(&info);
    uint64_t t = mach_absolute_time();
    return (double)(t * info.numer / info.denom) / 1e6;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
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
    int json_output = 0;
    const char *input_file = NULL;
    const char *load_libs[64];
    int num_load_libs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (strcmp(argv[i], "--json") == 0)
            json_output = 1;
        else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (num_load_libs < 64) load_libs[num_load_libs++] = argv[++i];
        }
        else if (argv[i][0] != '-')
            input_file = argv[i];
        else {
            fprintf(stderr, "usage: bench_llvm_jit [--iters N] [--json] [--load-lib LIB] file.ll\n");
            return 1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "usage: bench_llvm_jit [--iters N] [--json] [--load-lib LIB] file.ll\n");
        return 1;
    }

    for (int i = 0; i < num_load_libs; i++) {
        if (!dlopen(load_libs[i], RTLD_NOW | RTLD_GLOBAL)) {
            fprintf(stderr, "failed to load: %s: %s\n", load_libs[i], dlerror());
            return 1;
        }
    }

    size_t src_len;
    char *src = read_file(input_file, &src_len);
    if (!src) {
        fprintf(stderr, "failed to read %s\n", input_file);
        return 1;
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    double parse_total = 0, jit_total = 0;

    for (int iter = 0; iter < iters; iter++) {
        LLVMContextRef ctx = LLVMContextCreate();
        LLVMMemoryBufferRef buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(
            src, src_len, "input");
        LLVMModuleRef mod = NULL;
        char *err_msg = NULL;

        double t0 = now_ms();
        if (LLVMParseIRInContext(ctx, buf, &mod, &err_msg)) {
            if (json_output)
                printf("{\"file\":\"%s\",\"error\":\"parse: %s\"}\n",
                       input_file, err_msg ? err_msg : "unknown");
            else
                fprintf(stderr, "parse error: %s\n", err_msg ? err_msg : "unknown");
            LLVMDisposeMessage(err_msg);
            LLVMContextDispose(ctx);
            free(src);
            return 1;
        }
        double t1 = now_ms();
        parse_total += (t1 - t0);

        LLVMOrcLLJITRef jit = NULL;
        LLVMErrorRef e = LLVMOrcCreateLLJIT(&jit, NULL);
        if (e) {
            char *msg = LLVMGetErrorMessage(e);
            fprintf(stderr, "LLJIT create error: %s\n", msg);
            LLVMDisposeErrorMessage(msg);
            LLVMDisposeModule(mod);
            LLVMContextDispose(ctx);
            free(src);
            return 1;
        }

        LLVMOrcThreadSafeContextRef ts_ctx = LLVMOrcCreateNewThreadSafeContext();
        LLVMOrcThreadSafeModuleRef ts_mod =
            LLVMOrcCreateNewThreadSafeModule(mod, ts_ctx);

        double t2 = now_ms();
        LLVMOrcJITDylibRef dylib = LLVMOrcLLJITGetMainJITDylib(jit);
        e = LLVMOrcLLJITAddLLVMIRModule(jit, dylib, ts_mod);
        if (e) {
            char *msg = LLVMGetErrorMessage(e);
            if (json_output)
                printf("{\"file\":\"%s\",\"error\":\"jit: %s\"}\n", input_file, msg);
            else
                fprintf(stderr, "JIT error: %s\n", msg);
            LLVMDisposeErrorMessage(msg);
            LLVMOrcDisposeLLJIT(jit);
            LLVMOrcDisposeThreadSafeContext(ts_ctx);
            LLVMContextDispose(ctx);
            free(src);
            return 1;
        }

        LLVMOrcExecutorAddress addr = 0;
        e = LLVMOrcLLJITLookup(jit, &addr, "main");
        double t3 = now_ms();

        if (e) {
            LLVMDisposeErrorMessage(LLVMGetErrorMessage(e));
        }

        jit_total += (t3 - t2);

        LLVMOrcDisposeLLJIT(jit);
        LLVMOrcDisposeThreadSafeContext(ts_ctx);
        LLVMContextDispose(ctx);
    }

    double parse_avg = parse_total / iters;
    double jit_avg = jit_total / iters;
    double total_avg = parse_avg + jit_avg;
    double parse_pct = total_avg > 0 ? 100.0 * parse_avg / total_avg : 100.0;

    if (json_output) {
        printf("{\"file\":\"%s\",\"ll_bytes\":%zu,"
               "\"parse_ms\":%.3f,\"jit_ms\":%.3f,\"total_ms\":%.3f,"
               "\"parse_pct\":%.1f,\"iters\":%d}\n",
               input_file, src_len, parse_avg, jit_avg, total_avg,
               parse_pct, iters);
    } else {
        printf("file:      %s\n", input_file);
        printf("ll_bytes:  %zu\n", src_len);
        printf("parse:     %.3f ms (%.1f%%)\n", parse_avg, parse_pct);
        printf("jit:       %.3f ms (%.1f%%)\n", jit_avg, 100.0 - parse_pct);
        printf("total:     %.3f ms\n", total_avg);
        printf("iters:     %d\n", iters);
    }

    free(src);
    return 0;
}
