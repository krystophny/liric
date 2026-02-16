#include <liric/liric.h>
#include <liric/liric_legacy.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
static double now_us(void) {
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0) mach_timebase_info(&info);
    uint64_t t = mach_absolute_time();
    return (double)(t * info.numer / info.denom) / 1e3;
}
#else
#include <time.h>
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}
#endif

typedef struct {
    char *data;
    size_t len;
    int fd;
    int is_mmap;
} file_buf_t;

static int read_file(const char *path, file_buf_t *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }

    size_t len = (size_t)st.st_size;
    char *data = mmap(NULL, len + 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (data != MAP_FAILED) {
        data[len] = '\0';
        out->data = data;
        out->len = len;
        out->fd = fd;
        out->is_mmap = 1;
        return 0;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t nread = 0;
    while (nread < len) {
        ssize_t n = read(fd, buf + nread, len - nread);
        if (n <= 0) break;
        nread += (size_t)n;
    }
    buf[nread] = '\0';
    close(fd);

    out->data = buf;
    out->len = nread;
    out->fd = -1;
    out->is_mmap = 0;
    return 0;
}

static void free_file(file_buf_t *f) {
    if (f->is_mmap) {
        munmap(f->data, f->len + 1);
        close(f->fd);
    } else {
        free(f->data);
    }
}

static int backend_from_env(lr_backend_t *out_backend) {
    const char *mode = getenv("LIRIC_COMPILE_MODE");
    if (!out_backend)
        return -1;
    if (!mode || !mode[0] || strcmp(mode, "isel") == 0) {
        *out_backend = LR_BACKEND_ISEL;
        return 0;
    }
    if (strcmp(mode, "copy_patch") == 0 || strcmp(mode, "stencil") == 0) {
        *out_backend = LR_BACKEND_COPY_PATCH;
        return 0;
    }
    if (strcmp(mode, "llvm") == 0) {
        *out_backend = LR_BACKEND_LLVM;
        return 0;
    }
    return -1;
}

static int run_symbol_ptr(void *sym, const char *sig, int ignore_retcode) {
    char argv0[] = "liric";
    char *host_argv[] = {argv0, NULL};
    int host_argc = 1;

    if (strcmp(sig, "i32") == 0) {
        int32_t (*fn)(void) = NULL;
        int32_t ret = 0;
        memcpy(&fn, &sym, sizeof(sym));
        if (!fn) return 3;
        ret = fn();
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "i64") == 0) {
        int64_t (*fn)(void) = NULL;
        int64_t ret = 0;
        memcpy(&fn, &sym, sizeof(sym));
        if (!fn) return 3;
        ret = fn();
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "void") == 0) {
        void (*fn)(void) = NULL;
        memcpy(&fn, &sym, sizeof(sym));
        if (!fn) return 3;
        fn();
        return 0;
    }

    if (strcmp(sig, "i32_argc_argv") == 0) {
        int32_t (*fn)(int, char **) = NULL;
        int32_t ret = 0;
        memcpy(&fn, &sym, sizeof(sym));
        if (!fn) return 3;
        ret = fn(host_argc, host_argv);
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "i64_argc_argv") == 0) {
        int64_t (*fn)(int, char **) = NULL;
        int64_t ret = 0;
        memcpy(&fn, &sym, sizeof(sym));
        if (!fn) return 3;
        ret = fn(host_argc, host_argv);
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "void_argc_argv") == 0) {
        void (*fn)(int, char **) = NULL;
        memcpy(&fn, &sym, sizeof(sym));
        if (!fn) return 3;
        fn(host_argc, host_argv);
        return 0;
    }

    fprintf(stderr, "unsupported signature: %s\n", sig);
    return 2;
}

static void print_timing_line(int timing,
                              double t_read_start, double t_read_end,
                              double t_parse_start, double t_parse_end,
                              double t_jit_create_start, double t_jit_create_end,
                              double t_load_lib_start, double t_load_lib_end,
                              double t_compile_start, double t_compile_end,
                              double t_lookup_start, double t_lookup_end,
                              double t_exec_start, double t_exec_end) {
    if (!timing) return;
    double read_us = t_read_end - t_read_start;
    double parse_us = t_parse_end - t_parse_start;
    double jit_create_us = t_jit_create_end - t_jit_create_start;
    double load_lib_us = t_load_lib_end - t_load_lib_start;
    double compile_us = t_compile_end - t_compile_start;
    double lookup_us = t_lookup_end - t_lookup_start;
    double exec_us = t_exec_end - t_exec_start;
    double total_us = read_us + parse_us + jit_create_us + load_lib_us + compile_us + lookup_us + exec_us;
    fprintf(stderr, "TIMING read_us=%.3f parse_us=%.3f jit_create_us=%.3f "
            "load_lib_us=%.3f compile_us=%.3f lookup_us=%.3f exec_us=%.3f total_us=%.3f\n",
            read_us, parse_us, jit_create_us, load_lib_us, compile_us, lookup_us, exec_us, total_us);
}

int main(int argc, char **argv) {
    const char *func_name = "main";
    const char *sig = "i32";
    const char *input_file = NULL;
    const char *policy_arg = NULL;
    int ignore_retcode = 0;
    int timing = 0;
    int no_exec = 0;
    int parse_only = 0;
    const char *load_libs[64];
    int num_load_libs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) {
            func_name = argv[++i];
        } else if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) {
            sig = argv[++i];
        } else if (strcmp(argv[i], "--ignore-retcode") == 0) {
            ignore_retcode = 1;
        } else if (strcmp(argv[i], "--timing") == 0) {
            timing = 1;
        } else if (strcmp(argv[i], "--no-exec") == 0) {
            no_exec = 1;
        } else if (strcmp(argv[i], "--parse-only") == 0) {
            parse_only = 1;
        } else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (num_load_libs < 64) {
                load_libs[num_load_libs++] = argv[++i];
            } else {
                fprintf(stderr, "too many --load-lib options\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            policy_arg = argv[++i];
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

    if (policy_arg && strcmp(policy_arg, "direct") != 0 &&
        strcmp(policy_arg, "ir") != 0) {
        fprintf(stderr, "invalid --policy value: %s (expected direct|ir)\n",
                policy_arg);
        return 1;
    }

    double t_read_start = 0, t_read_end = 0;
    double t_parse_start = 0, t_parse_end = 0;
    double t_jit_create_start = 0, t_jit_create_end = 0;
    double t_load_lib_start = 0, t_load_lib_end = 0;
    double t_compile_start = 0, t_compile_end = 0;
    double t_lookup_start = 0, t_lookup_end = 0;
    double t_exec_start = 0, t_exec_end = 0;

    if (timing) t_read_start = now_us();
    file_buf_t src = {0};
    if (read_file(input_file, &src) != 0) {
        if (timing) t_read_end = now_us();
        print_timing_line(timing, t_read_start, t_read_end,
                          t_parse_start, t_parse_end,
                          t_jit_create_start, t_jit_create_end,
                          t_load_lib_start, t_load_lib_end,
                          t_compile_start, t_compile_end,
                          t_lookup_start, t_lookup_end,
                          t_exec_start, t_exec_end);
        fprintf(stderr, "failed to read input file\n");
        return 1;
    }
    if (timing) t_read_end = now_us();

    if (parse_only) {
        char parse_err[256] = {0};
        lr_module_t *parsed = NULL;
        if (timing) t_parse_start = now_us();
        parsed = lr_parse_auto((const uint8_t *)src.data, src.len,
                               parse_err, sizeof(parse_err));
        if (timing) t_parse_end = now_us();
        if (!parsed) {
            print_timing_line(timing, t_read_start, t_read_end,
                              t_parse_start, t_parse_end,
                              t_jit_create_start, t_jit_create_end,
                              t_load_lib_start, t_load_lib_end,
                              t_compile_start, t_compile_end,
                              t_lookup_start, t_lookup_end,
                              t_exec_start, t_exec_end);
            fprintf(stderr, "parse failed: %s\n",
                    parse_err[0] ? parse_err : "unknown error");
            free_file(&src);
            return 1;
        }
        if (timing) {
            t_compile_start = t_parse_end;
            t_compile_end = t_parse_end;
        }
        lr_module_free(parsed);
        print_timing_line(timing, t_read_start, t_read_end,
                          t_parse_start, t_parse_end,
                          t_jit_create_start, t_jit_create_end,
                          t_load_lib_start, t_load_lib_end,
                          t_compile_start, t_compile_end,
                          t_lookup_start, t_lookup_end,
                          t_exec_start, t_exec_end);
        free_file(&src);
        return 0;
    }

    lr_backend_t backend = LR_BACKEND_ISEL;
    lr_policy_t policy = LR_POLICY_DIRECT;
    lr_compiler_config_t cfg = {0};
    lr_compiler_error_t cerr = {0};
    lr_compiler_t *compiler = NULL;
    if (backend_from_env(&backend) != 0) {
        print_timing_line(timing, t_read_start, t_read_end,
                          t_parse_start, t_parse_end,
                          t_jit_create_start, t_jit_create_end,
                          t_load_lib_start, t_load_lib_end,
                          t_compile_start, t_compile_end,
                          t_lookup_start, t_lookup_end,
                          t_exec_start, t_exec_end);
        fprintf(stderr, "invalid LIRIC_COMPILE_MODE value\n");
        free_file(&src);
        return 1;
    }

    if (backend == LR_BACKEND_LLVM)
        policy = LR_POLICY_IR;
    if (policy_arg) {
        policy = (strcmp(policy_arg, "ir") == 0) ? LR_POLICY_IR
                                                  : LR_POLICY_DIRECT;
    }

    cfg.policy = policy;
    cfg.backend = backend;
    cfg.target = NULL;

    if (timing) t_jit_create_start = now_us();
    compiler = lr_compiler_create(&cfg, &cerr);
    if (!compiler) {
        if (timing) t_jit_create_end = now_us();
        print_timing_line(timing, t_read_start, t_read_end,
                          t_parse_start, t_parse_end,
                          t_jit_create_start, t_jit_create_end,
                          t_load_lib_start, t_load_lib_end,
                          t_compile_start, t_compile_end,
                          t_lookup_start, t_lookup_end,
                          t_exec_start, t_exec_end);
        fprintf(stderr, "compiler create failed: %s\n",
                cerr.msg[0] ? cerr.msg : "unknown error");
        free_file(&src);
        return 1;
    }
    if (timing) t_jit_create_end = now_us();

    if (timing) t_load_lib_start = now_us();
    for (int i = 0; i < num_load_libs; i++) {
        if (lr_compiler_load_library(compiler, load_libs[i], &cerr) != 0) {
            if (timing) t_load_lib_end = now_us();
            print_timing_line(timing, t_read_start, t_read_end,
                              t_parse_start, t_parse_end,
                              t_jit_create_start, t_jit_create_end,
                              t_load_lib_start, t_load_lib_end,
                              t_compile_start, t_compile_end,
                              t_lookup_start, t_lookup_end,
                              t_exec_start, t_exec_end);
            fprintf(stderr, "failed to load library '%s': %s\n", load_libs[i],
                    cerr.msg[0] ? cerr.msg : "unknown error");
            lr_compiler_destroy(compiler);
            free_file(&src);
            return 1;
        }
    }
    if (timing) t_load_lib_end = now_us();

    if (timing) t_parse_start = now_us();
    if (lr_compiler_feed_auto(compiler, (const uint8_t *)src.data, src.len, &cerr) != 0) {
        if (timing) t_parse_end = now_us();
        print_timing_line(timing, t_read_start, t_read_end,
                          t_parse_start, t_parse_end,
                          t_jit_create_start, t_jit_create_end,
                          t_load_lib_start, t_load_lib_end,
                          t_compile_start, t_compile_end,
                          t_lookup_start, t_lookup_end,
                          t_exec_start, t_exec_end);
        fprintf(stderr, "streaming compile failed: %s\n",
                cerr.msg[0] ? cerr.msg : "unknown error");
        lr_compiler_destroy(compiler);
        free_file(&src);
        return 1;
    }
    if (timing) t_parse_end = now_us();
    if (timing) {
        t_compile_start = t_parse_end;
        t_compile_end = t_parse_end;
    }

    if (timing) t_lookup_start = now_us();
    void *sym = lr_compiler_lookup(compiler, func_name);
    if (timing) t_lookup_end = now_us();
    if (!sym) {
        print_timing_line(timing, t_read_start, t_read_end,
                          t_parse_start, t_parse_end,
                          t_jit_create_start, t_jit_create_end,
                          t_load_lib_start, t_load_lib_end,
                          t_compile_start, t_compile_end,
                          t_lookup_start, t_lookup_end,
                          t_exec_start, t_exec_end);
        fprintf(stderr, "function '%s' not found\n", func_name);
        lr_compiler_destroy(compiler);
        free_file(&src);
        return 3;
    }

    int run_rc = 0;
    if (!no_exec) {
        if (timing) t_exec_start = now_us();
        run_rc = run_symbol_ptr(sym, sig, ignore_retcode);
        if (timing) t_exec_end = now_us();
    }

    print_timing_line(timing, t_read_start, t_read_end,
                      t_parse_start, t_parse_end,
                      t_jit_create_start, t_jit_create_end,
                      t_load_lib_start, t_load_lib_end,
                      t_compile_start, t_compile_end,
                      t_lookup_start, t_lookup_end,
                      t_exec_start, t_exec_end);

    lr_compiler_destroy(compiler);
    free_file(&src);
    return run_rc;
}
