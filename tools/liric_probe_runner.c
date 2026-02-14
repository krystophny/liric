#include "../src/ir.h"
#include "../src/jit.h"
#include "../src/liric.h"

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

static int run_symbol_ptr(void *sym, const char *sig, int ignore_retcode) {
    char argv0[] = "liric";
    char *host_argv[] = {argv0, NULL};
    int host_argc = 1;

    if (strcmp(sig, "i32") == 0) {
        int32_t (*fn)(void) = NULL;
        int32_t ret = 0;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) return 3;
        ret = fn();
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "i64") == 0) {
        int64_t (*fn)(void) = NULL;
        int64_t ret = 0;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) return 3;
        ret = fn();
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "void") == 0) {
        void (*fn)(void) = NULL;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) return 3;
        fn();
        return 0;
    }

    if (strcmp(sig, "i32_argc_argv") == 0) {
        int32_t (*fn)(int, char **) = NULL;
        int32_t ret = 0;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) return 3;
        ret = fn(host_argc, host_argv);
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "i64_argc_argv") == 0) {
        int64_t (*fn)(int, char **) = NULL;
        int64_t ret = 0;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) return 3;
        ret = fn(host_argc, host_argv);
        return ignore_retcode ? 0 : (int)(ret & 0xff);
    }

    if (strcmp(sig, "void_argc_argv") == 0) {
        void (*fn)(int, char **) = NULL;
        lr_jit_fn_to_ptr(&fn, sym);
        if (!fn) return 3;
        fn(host_argc, host_argv);
        return 0;
    }

    fprintf(stderr, "unsupported signature: %s\n", sig);
    return 2;
}

int main(int argc, char **argv) {
    const char *func_name = "main";
    const char *sig = "i32";
    const char *input_file = NULL;
    const char *runtime_bc_file = NULL;
    int ignore_retcode = 0;
    int timing = 0;
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
        } else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (num_load_libs < 64) {
                load_libs[num_load_libs++] = argv[++i];
            } else {
                fprintf(stderr, "too many --load-lib options\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--runtime-bc") == 0 && i + 1 < argc) {
            runtime_bc_file = argv[++i];
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
        fprintf(stderr, "failed to read input file\n");
        return 1;
    }

    file_buf_t runtime_bc = {0};
    int have_runtime_bc = 0;
    if (runtime_bc_file) {
        if (read_file(runtime_bc_file, &runtime_bc) != 0) {
            fprintf(stderr, "failed to read runtime bc file: %s\n", runtime_bc_file);
            free_file(&src);
            return 1;
        }
        have_runtime_bc = 1;
    }
    if (timing) t_read_end = now_us();

    if (timing) t_parse_start = now_us();
    char err[512] = {0};
    lr_module_t *m = lr_parse_ll(src.data, src.len, err, sizeof(err));
    if (!m) {
        fprintf(stderr, "parse error: %s\n", err);
        if (have_runtime_bc)
            free_file(&runtime_bc);
        free_file(&src);
        return 1;
    }
    if (timing) t_parse_end = now_us();

    if (timing) t_jit_create_start = now_us();
    lr_jit_t *jit = lr_jit_create();
    if (!jit) {
        fprintf(stderr, "failed to create JIT\n");
        lr_module_free(m);
        if (have_runtime_bc)
            free_file(&runtime_bc);
        free_file(&src);
        return 1;
    }
    if (timing) t_jit_create_end = now_us();

    if (have_runtime_bc) {
        if (!getenv("LIRIC_JIT_LAZY"))
            setenv("LIRIC_JIT_LAZY", "1", 0);
        lr_jit_set_runtime_bc(jit, (const uint8_t *)runtime_bc.data, runtime_bc.len);
    }

    if (timing) t_load_lib_start = now_us();
    for (int i = 0; i < num_load_libs; i++) {
        if (lr_jit_load_library(jit, load_libs[i]) != 0) {
            fprintf(stderr, "failed to load library: %s\n", load_libs[i]);
            lr_jit_destroy(jit);
            lr_module_free(m);
            if (have_runtime_bc)
                free_file(&runtime_bc);
            free_file(&src);
            return 1;
        }
    }
    if (timing) t_load_lib_end = now_us();

    if (timing) t_compile_start = now_us();
    int rc = lr_jit_add_module(jit, m);
    if (rc != 0) {
        fprintf(stderr, "JIT compilation failed\n");
        lr_jit_destroy(jit);
        lr_module_free(m);
        if (have_runtime_bc)
            free_file(&runtime_bc);
        free_file(&src);
        return 1;
    }
    if (timing) t_compile_end = now_us();

    if (timing) t_lookup_start = now_us();
    void *sym = lr_jit_get_function(jit, func_name);
    if (timing) t_lookup_end = now_us();
    if (!sym) {
        fprintf(stderr, "function '%s' not found\n", func_name);
        lr_jit_destroy(jit);
        lr_module_free(m);
        if (have_runtime_bc)
            free_file(&runtime_bc);
        free_file(&src);
        return 3;
    }

    if (timing) t_exec_start = now_us();
    int run_rc = run_symbol_ptr(sym, sig, ignore_retcode);
    if (timing) t_exec_end = now_us();

    if (timing) {
        double read_us = t_read_end - t_read_start;
        double parse_us = t_parse_end - t_parse_start;
        double jit_create_us = t_jit_create_end - t_jit_create_start;
        double load_lib_us = t_load_lib_end - t_load_lib_start;
        double compile_us = t_compile_end - t_compile_start;
        double lookup_us = t_lookup_end - t_lookup_start;
        double exec_us = t_exec_end - t_exec_start;
        double total_us = read_us + parse_us + jit_create_us + load_lib_us + compile_us + lookup_us + exec_us;
        fprintf(stderr, "TIMING read_us=%.1f parse_us=%.1f jit_create_us=%.1f "
                "load_lib_us=%.1f compile_us=%.1f lookup_us=%.1f exec_us=%.1f total_us=%.1f\n",
                read_us, parse_us, jit_create_us, load_lib_us, compile_us, lookup_us, exec_us, total_us);
    }

    lr_jit_destroy(jit);
    lr_module_free(m);
    if (have_runtime_bc)
        free_file(&runtime_bc);
    free_file(&src);
    return run_rc;
}
