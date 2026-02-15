// Fair in-process LLVM ORC phase benchmark for .ll inputs.
// Measures parse, JIT compile, symbol lookup, and execution in one process.

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

typedef struct {
    const char *func_name;
    const char *sig;
    const char *input_file;
    int iters;
    int json_output;
    int no_exec;
    int parse_only;
    const char *load_libs[64];
    int num_load_libs;
} args_t;

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long len;
    size_t n;
    char *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static int run_symbol(LLVMOrcExecutorAddress addr, const char *sig, int *retcode_out) {
    char argv0[] = "bench_lli_phases";
    char *host_argv[] = {argv0, NULL};
    int host_argc = 1;

    if (strcmp(sig, "i32") == 0) {
        int32_t (*fn)(void) = NULL;
        int32_t ret;
        fn = (int32_t (*)(void))(uintptr_t)addr;
        if (!fn) return 1;
        ret = fn();
        *retcode_out = (int)(ret & 0xff);
        return 0;
    }

    if (strcmp(sig, "i64") == 0) {
        int64_t (*fn)(void) = NULL;
        int64_t ret;
        fn = (int64_t (*)(void))(uintptr_t)addr;
        if (!fn) return 1;
        ret = fn();
        *retcode_out = (int)(ret & 0xff);
        return 0;
    }

    if (strcmp(sig, "void") == 0) {
        void (*fn)(void) = NULL;
        fn = (void (*)(void))(uintptr_t)addr;
        if (!fn) return 1;
        fn();
        *retcode_out = 0;
        return 0;
    }

    if (strcmp(sig, "i32_argc_argv") == 0) {
        int32_t (*fn)(int, char **) = NULL;
        int32_t ret;
        fn = (int32_t (*)(int, char **))(uintptr_t)addr;
        if (!fn) return 1;
        ret = fn(host_argc, host_argv);
        *retcode_out = (int)(ret & 0xff);
        return 0;
    }

    if (strcmp(sig, "i64_argc_argv") == 0) {
        int64_t (*fn)(int, char **) = NULL;
        int64_t ret;
        fn = (int64_t (*)(int, char **))(uintptr_t)addr;
        if (!fn) return 1;
        ret = fn(host_argc, host_argv);
        *retcode_out = (int)(ret & 0xff);
        return 0;
    }

    if (strcmp(sig, "void_argc_argv") == 0) {
        void (*fn)(int, char **) = NULL;
        fn = (void (*)(int, char **))(uintptr_t)addr;
        if (!fn) return 1;
        fn(host_argc, host_argv);
        *retcode_out = 0;
        return 0;
    }

    return 2;
}

static int parse_args(int argc, char **argv, args_t *a) {
    int i;
    a->func_name = "main";
    a->sig = "i32";
    a->input_file = NULL;
    a->iters = 1;
    a->json_output = 0;
    a->no_exec = 0;
    a->parse_only = 0;
    a->num_load_libs = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            a->iters = atoi(argv[++i]);
            if (a->iters <= 0) a->iters = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            a->json_output = 1;
        } else if (strcmp(argv[i], "--no-exec") == 0) {
            a->no_exec = 1;
        } else if (strcmp(argv[i], "--parse-only") == 0) {
            a->parse_only = 1;
        } else if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) {
            a->func_name = argv[++i];
        } else if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) {
            a->sig = argv[++i];
        } else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (a->num_load_libs >= 64) {
                fprintf(stderr, "too many --load-lib entries\n");
                return 1;
            }
            a->load_libs[a->num_load_libs++] = argv[++i];
        } else if (argv[i][0] != '-') {
            a->input_file = argv[i];
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!a->input_file) {
        fprintf(stderr,
            "usage: bench_lli_phases [--iters N] [--json] [--func NAME] [--sig SIG] "
            "[--load-lib LIB] [--no-exec] [--parse-only] file.ll\n");
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    args_t a;
    size_t src_len = 0;
    char *src = NULL;
    int i;
    double parse_input_total = 0.0;
    double jit_total = 0.0;
    double lookup_total = 0.0;
    double exec_total = 0.0;
    int retcode_last = 0;

    if (parse_args(argc, argv, &a) != 0) return 1;

    for (i = 0; i < a.num_load_libs; i++) {
        if (!dlopen(a.load_libs[i], RTLD_NOW | RTLD_GLOBAL)) {
            fprintf(stderr, "failed to load: %s: %s\n", a.load_libs[i], dlerror());
            return 1;
        }
    }

    src = read_file(a.input_file, &src_len);
    if (!src) {
        fprintf(stderr, "failed to read %s\n", a.input_file);
        return 1;
    }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    for (i = 0; i < a.iters; i++) {
        LLVMContextRef ctx = NULL;
        LLVMMemoryBufferRef buf = NULL;
        LLVMModuleRef mod = NULL;
        LLVMOrcLLJITRef jit = NULL;
        LLVMOrcThreadSafeContextRef ts_ctx = NULL;
        LLVMOrcThreadSafeModuleRef ts_mod = NULL;
        LLVMOrcJITDylibRef dylib;
        LLVMOrcExecutorAddress addr = 0;
        LLVMErrorRef err;
        char *err_msg = NULL;
        double t0, t1;
        int run_rc;

        ctx = LLVMContextCreate();
        if (!ctx) {
            fprintf(stderr, "failed to create LLVM context\n");
            free(src);
            return 1;
        }

        buf = LLVMCreateMemoryBufferWithMemoryRangeCopy(src, src_len, "input");
        if (!buf) {
            fprintf(stderr, "failed to create memory buffer\n");
            LLVMContextDispose(ctx);
            free(src);
            return 1;
        }

        t0 = now_ms();
        if (LLVMParseIRInContext(ctx, buf, &mod, &err_msg)) {
            fprintf(stderr, "parse error: %s\n", err_msg ? err_msg : "unknown");
            if (err_msg) LLVMDisposeMessage(err_msg);
            LLVMContextDispose(ctx);
            free(src);
            return 1;
        }
        t1 = now_ms();
        parse_input_total += (t1 - t0);

        if (!a.parse_only) {
            err = LLVMOrcCreateLLJIT(&jit, NULL);
            if (err) {
                char *msg = LLVMGetErrorMessage(err);
                fprintf(stderr, "LLJIT create error: %s\n", msg ? msg : "unknown");
                if (msg) LLVMDisposeErrorMessage(msg);
                LLVMDisposeModule(mod);
                LLVMContextDispose(ctx);
                free(src);
                return 1;
            }

            ts_ctx = LLVMOrcCreateNewThreadSafeContext();
            if (!ts_ctx) {
                fprintf(stderr, "failed to create thread-safe context\n");
                LLVMOrcDisposeLLJIT(jit);
                LLVMDisposeModule(mod);
                LLVMContextDispose(ctx);
                free(src);
                return 1;
            }
            ts_mod = LLVMOrcCreateNewThreadSafeModule(mod, ts_ctx);

            t0 = now_ms();
            dylib = LLVMOrcLLJITGetMainJITDylib(jit);
            err = LLVMOrcLLJITAddLLVMIRModule(jit, dylib, ts_mod);
            if (err) {
                char *msg = LLVMGetErrorMessage(err);
                fprintf(stderr, "JIT error: %s\n", msg ? msg : "unknown");
                if (msg) LLVMDisposeErrorMessage(msg);
                LLVMOrcDisposeLLJIT(jit);
                LLVMOrcDisposeThreadSafeContext(ts_ctx);
                LLVMContextDispose(ctx);
                free(src);
                return 1;
            }
            t1 = now_ms();
            jit_total += (t1 - t0);

            t0 = now_ms();
            err = LLVMOrcLLJITLookup(jit, &addr, a.func_name);
            t1 = now_ms();
            lookup_total += (t1 - t0);
            if (err) {
                char *msg = LLVMGetErrorMessage(err);
                fprintf(stderr, "lookup error (%s): %s\n", a.func_name, msg ? msg : "unknown");
                if (msg) LLVMDisposeErrorMessage(msg);
                LLVMOrcDisposeLLJIT(jit);
                LLVMOrcDisposeThreadSafeContext(ts_ctx);
                LLVMContextDispose(ctx);
                free(src);
                return 1;
            }

            if (!a.no_exec) {
                t0 = now_ms();
                run_rc = run_symbol(addr, a.sig, &retcode_last);
                t1 = now_ms();
                exec_total += (t1 - t0);
                if (run_rc != 0) {
                    if (run_rc == 2) {
                        fprintf(stderr, "unsupported signature: %s\n", a.sig);
                    } else {
                        fprintf(stderr, "failed to run function '%s'\n", a.func_name);
                    }
                    LLVMOrcDisposeLLJIT(jit);
                    LLVMOrcDisposeThreadSafeContext(ts_ctx);
                    LLVMContextDispose(ctx);
                    free(src);
                    return 1;
                }
            }

            LLVMOrcDisposeLLJIT(jit);
            LLVMOrcDisposeThreadSafeContext(ts_ctx);
        } else {
            LLVMDisposeModule(mod);
        }
        LLVMContextDispose(ctx);
    }

    double iters_d = (double)a.iters;
    double avg_parse_input = parse_input_total / iters_d;
    double avg_parse = avg_parse_input;
    double avg_add = jit_total / iters_d;
    double avg_lookup = lookup_total / iters_d;
    double avg_exec = exec_total / iters_d;
    double avg_compile = avg_add + avg_lookup;
    double avg_total = avg_parse + avg_add + avg_lookup + avg_exec;
    double avg_compile_materialized = avg_add + avg_lookup;

    if (a.json_output) {
        printf("{\"file\":\"%s\",\"iters\":%d,"
               "\"parse_input_ms\":%.6f,\"parse_ms\":%.6f,"
               "\"add_module_ms\":%.6f,\"lookup_ms\":%.6f,"
               "\"compile_materialized_ms\":%.6f,\"compile_ms\":%.6f,\"exec_ms\":%.6f,"
               "\"total_ms\":%.6f,\"retcode\":%d}\n",
               a.input_file, a.iters,
               avg_parse_input, avg_parse, avg_add, avg_lookup,
               avg_compile_materialized, avg_compile, avg_exec,
               avg_total, retcode_last);
    } else {
        printf("file:       %s\n", a.input_file);
        printf("iters:      %d\n", a.iters);
        printf("parse:      %.6f ms\n", avg_parse);
        printf("add_module: %.6f ms  (lazy registration)\n", avg_add);
        printf("lookup:     %.6f ms  (triggers lazy compile)\n", avg_lookup);
        printf("compile:    %.6f ms  (add_module + lookup)\n", avg_compile_materialized);
        printf("exec:       %.6f ms\n", avg_exec);
        printf("total:      %.6f ms\n", avg_total);
        printf("retcode:    %d\n", retcode_last);
    }

    free(src);
    return 0;
}
