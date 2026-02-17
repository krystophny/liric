#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PATH_MAX_LOCAL 4096

#define SKIP_LABEL_1 "llvm_omp"
#define SKIP_LABEL_2 "llvm2"
#define SKIP_LABEL_3 "llvm_rtlib"

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} str_vec_t;

typedef struct {
    double *items;
    size_t count;
    size_t cap;
} dbl_vec_t;

typedef struct {
    char *name;
    char *source_path;
    str_vec_t options;
} bench_test_t;

typedef struct {
    bench_test_t *items;
    size_t count;
    size_t cap;
} test_vec_t;

typedef struct {
    int rc;
    double wall_ms;
    char *out;
    char *err;
} cmd_result_t;

typedef struct {
    bool have;
    double read_us;
    double parse_us;
    double jit_create_us;
    double load_lib_us;
    double compile_us;
    double run_us;
    double total_us;
} probe_timing_t;

typedef struct {
    char *name;
    bool api_exe_ok;
    bool api_jit_ok;
    bool ll_jit_ok;
    bool ll_lli_ok;
    bool api_jit_match;
    bool ll_jit_match;
    bool ll_lli_match;

    double api_exe_compile_ms;
    double api_exe_run_ms;
    double api_exe_wall_ms;
    double api_exe_non_parse_ms;

    double api_jit_emit_ms;
    double api_jit_wall_ms;
    double api_jit_parse_ms;
    double api_jit_compile_ms;
    double api_jit_run_ms;
    double api_jit_non_parse_ms;

    double ll_jit_wall_ms;
    double ll_jit_parse_ms;
    double ll_jit_compile_ms;
    double ll_jit_run_ms;
    double ll_jit_non_parse_ms;

    double ll_lli_wall_ms;
} bench_row_t;

typedef struct {
    bench_row_t *items;
    size_t count;
    size_t cap;
} row_vec_t;

typedef struct {
    const char *bench_dir;
    const char *integration_cmake;
    const char *integration_dir;
    const char *lfortran;
    const char *probe_runner;
    const char *runtime_lib;
    const char *lli;
    int iters;
    int timeout_sec;
    int limit;
} cfg_t;

static double now_ms(void) {
#if defined(__APPLE__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "ERROR: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) die("out of memory");
    memcpy(p, s, n + 1);
    return p;
}

static void str_vec_push(str_vec_t *v, const char *s) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 8;
        char **tmp = (char **)realloc(v->items, ncap * sizeof(char *));
        if (!tmp) die("out of memory");
        v->items = tmp;
        v->cap = ncap;
    }
    v->items[v->count++] = xstrdup(s);
}

static void str_vec_free(str_vec_t *v) {
    size_t i;
    for (i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static bool str_vec_contains(const str_vec_t *v, const char *needle) {
    size_t i;
    for (i = 0; i < v->count; i++) {
        if (strcmp(v->items[i], needle) == 0) return true;
    }
    return false;
}

static void dbl_vec_push(dbl_vec_t *v, double x) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 16;
        double *tmp = (double *)realloc(v->items, ncap * sizeof(double));
        if (!tmp) die("out of memory");
        v->items = tmp;
        v->cap = ncap;
    }
    v->items[v->count++] = x;
}

static void dbl_vec_free(dbl_vec_t *v) {
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static void test_vec_push(test_vec_t *v, const bench_test_t *t) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        bench_test_t *tmp = (bench_test_t *)realloc(v->items, ncap * sizeof(bench_test_t));
        if (!tmp) die("out of memory");
        v->items = tmp;
        v->cap = ncap;
    }
    v->items[v->count++] = *t;
}

static void test_vec_free(test_vec_t *v) {
    size_t i;
    for (i = 0; i < v->count; i++) {
        free(v->items[i].name);
        free(v->items[i].source_path);
        str_vec_free(&v->items[i].options);
    }
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static void row_vec_push(row_vec_t *v, const bench_row_t *r) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        bench_row_t *tmp = (bench_row_t *)realloc(v->items, ncap * sizeof(bench_row_t));
        if (!tmp) die("out of memory");
        v->items = tmp;
        v->cap = ncap;
    }
    v->items[v->count++] = *r;
}

static void row_vec_free(row_vec_t *v) {
    size_t i;
    for (i = 0; i < v->count; i++) {
        free(v->items[i].name);
    }
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static void mkdir_p(const char *path) {
    char buf[PATH_MAX_LOCAL];
    size_t n = strlen(path);
    size_t i;
    if (n == 0 || n >= sizeof(buf)) die("bad path: %s", path);
    memcpy(buf, path, n + 1);

    for (i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (buf[0] != '\0' && !dir_exists(buf)) {
                if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                    die("mkdir failed: %s", buf);
                }
            }
            buf[i] = '/';
        }
    }
    if (!dir_exists(buf)) {
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            die("mkdir failed: %s", buf);
        }
    }
}

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    size_t nread;
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
    if (!buf) die("out of memory");
    nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

static void build_path(char *dst, size_t cap, const char *a, const char *b) {
    if (snprintf(dst, cap, "%s/%s", a, b) >= (int)cap) {
        die("path too long");
    }
}

static char *dirname_copy(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    {
        size_t n = (size_t)(slash - path);
        char *out = (char *)malloc(n + 1);
        if (!out) die("out of memory");
        memcpy(out, path, n);
        out[n] = '\0';
        return out;
    }
}

static char *strip_comments(const char *src) {
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    size_t i = 0, w = 0;
    bool in_quote = false;
    if (!out) die("out of memory");

    while (i < n) {
        char c = src[i];
        if (c == '"') {
            in_quote = !in_quote;
            out[w++] = c;
            i++;
            continue;
        }
        if (c == '#' && !in_quote) {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        out[w++] = c;
        i++;
    }
    out[w] = '\0';
    return out;
}

static char *substr_dup(const char *s, size_t start, size_t end) {
    size_t n;
    char *out;
    if (end < start) end = start;
    n = end - start;
    out = (char *)malloc(n + 1);
    if (!out) die("out of memory");
    memcpy(out, s + start, n);
    out[n] = '\0';
    return out;
}

static void tokenize_block(const char *block, str_vec_t *tokens) {
    size_t i = 0;
    size_t n = strlen(block);
    while (i < n) {
        while (i < n && isspace((unsigned char)block[i])) i++;
        if (i >= n) break;
        if (block[i] == '"') {
            size_t j = i + 1;
            while (j < n && block[j] != '"') {
                if (block[j] == '\\' && j + 1 < n) j += 2;
                else j++;
            }
            if (j >= n) {
                str_vec_push(tokens, block + i + 1);
                break;
            }
            {
                char *tmp = substr_dup(block, i + 1, j);
                str_vec_push(tokens, tmp);
                free(tmp);
            }
            i = j + 1;
        } else {
            size_t j = i;
            while (j < n && !isspace((unsigned char)block[j])) j++;
            {
                char *tmp = substr_dup(block, i, j);
                str_vec_push(tokens, tmp);
                free(tmp);
            }
            i = j;
        }
    }
}

typedef struct {
    char *name;
    char *file;
    char *include_path;
    bool fail;
    str_vec_t labels;
    str_vec_t extrafiles;
    str_vec_t extra_args;
} run_entry_t;

static void run_entry_free(run_entry_t *e) {
    free(e->name);
    free(e->file);
    free(e->include_path);
    str_vec_free(&e->labels);
    str_vec_free(&e->extrafiles);
    str_vec_free(&e->extra_args);
}

static bool is_one_value_key(const char *tok) {
    return strcmp(tok, "NAME") == 0 || strcmp(tok, "FILE") == 0 ||
           strcmp(tok, "INCLUDE_PATH") == 0 || strcmp(tok, "COPY_TO_BIN") == 0;
}

static bool is_multi_key(const char *tok) {
    return strcmp(tok, "LABELS") == 0 || strcmp(tok, "EXTRAFILES") == 0 ||
           strcmp(tok, "EXTRA_ARGS") == 0 || strcmp(tok, "GFORTRAN_ARGS") == 0;
}

static bool is_flag_key(const char *tok) {
    return strcmp(tok, "FAIL") == 0 || strcmp(tok, "NOFAST_TILL_LLVM16") == 0 ||
           strcmp(tok, "NO_FAST") == 0 || strcmp(tok, "NO_STD_F23") == 0 ||
           strcmp(tok, "OLD_CLASSES") == 0 || strcmp(tok, "NO_LLVM_GOC") == 0;
}

static run_entry_t parse_run_block(const char *block) {
    run_entry_t e;
    str_vec_t toks = {0};
    size_t i = 0;
    enum { M_NONE, M_LABELS, M_EXTRAFILES, M_EXTRA_ARGS } mode = M_NONE;

    memset(&e, 0, sizeof(e));
    tokenize_block(block, &toks);

    while (i < toks.count) {
        const char *tok = toks.items[i];
        if (is_flag_key(tok)) {
            if (strcmp(tok, "FAIL") == 0) e.fail = true;
            mode = M_NONE;
            i++;
            continue;
        }
        if (is_one_value_key(tok)) {
            if (i + 1 < toks.count) {
                const char *val = toks.items[i + 1];
                if (strcmp(tok, "NAME") == 0) {
                    free(e.name);
                    e.name = xstrdup(val);
                } else if (strcmp(tok, "FILE") == 0) {
                    free(e.file);
                    e.file = xstrdup(val);
                } else if (strcmp(tok, "INCLUDE_PATH") == 0) {
                    free(e.include_path);
                    e.include_path = xstrdup(val);
                }
                i += 2;
            } else {
                i++;
            }
            mode = M_NONE;
            continue;
        }
        if (is_multi_key(tok)) {
            if (strcmp(tok, "LABELS") == 0) mode = M_LABELS;
            else if (strcmp(tok, "EXTRAFILES") == 0) mode = M_EXTRAFILES;
            else if (strcmp(tok, "EXTRA_ARGS") == 0) mode = M_EXTRA_ARGS;
            else mode = M_NONE;
            i++;
            continue;
        }

        if (mode == M_LABELS) str_vec_push(&e.labels, tok);
        else if (mode == M_EXTRAFILES) str_vec_push(&e.extrafiles, tok);
        else if (mode == M_EXTRA_ARGS) str_vec_push(&e.extra_args, tok);

        i++;
    }

    str_vec_free(&toks);
    return e;
}

static void append_unique(str_vec_t *v, const char *s) {
    if (!str_vec_contains(v, s)) str_vec_push(v, s);
}

static void apply_label_options(const str_vec_t *labels, str_vec_t *options) {
    if (str_vec_contains(labels, "llvmImplicit")) {
        append_unique(options, "--implicit-typing");
        append_unique(options, "--implicit-interface");
    }
    if (str_vec_contains(labels, "llvmStackArray")) {
        append_unique(options, "--stack-arrays=true");
    }
    if (str_vec_contains(labels, "llvm_integer_8")) {
        append_unique(options, "-fdefault-integer-8");
    }
    if (str_vec_contains(labels, "llvm_nopragma")) {
        append_unique(options, "--ignore-pragma");
    }
}

static bool has_suffix(const char *s, const char *suffix) {
    size_t ns = strlen(s), nx = strlen(suffix);
    if (ns < nx) return false;
    return strcmp(s + ns - nx, suffix) == 0;
}

static bool file_token_has_ext(const char *s) {
    const char *base = strrchr(s, '/');
    if (!base) base = s;
    else base++;
    return strchr(base, '.') != NULL;
}

static void add_test_from_entry(const run_entry_t *e, const char *integration_dir, test_vec_t *out) {
    char src_rel[PATH_MAX_LOCAL];
    char src_path[PATH_MAX_LOCAL];
    bench_test_t t;

    if (!e->name || e->name[0] == '\0') return;
    if (e->fail) return;
    if (!str_vec_contains(&e->labels, "llvm")) return;
    if (str_vec_contains(&e->labels, SKIP_LABEL_1)) return;
    if (str_vec_contains(&e->labels, SKIP_LABEL_2)) return;
    if (str_vec_contains(&e->labels, SKIP_LABEL_3)) return;
    if (e->extrafiles.count > 0) return;

    if (e->file && e->file[0] != '\0') {
        if (file_token_has_ext(e->file)) {
            if (snprintf(src_rel, sizeof(src_rel), "%s", e->file) >= (int)sizeof(src_rel)) return;
        } else {
            if (snprintf(src_rel, sizeof(src_rel), "%s.f90", e->file) >= (int)sizeof(src_rel)) return;
        }
    } else {
        if (snprintf(src_rel, sizeof(src_rel), "%s.f90", e->name) >= (int)sizeof(src_rel)) return;
    }

    if (snprintf(src_path, sizeof(src_path), "%s/%s", integration_dir, src_rel) >= (int)sizeof(src_path)) return;
    if (!file_exists(src_path)) return;

    memset(&t, 0, sizeof(t));
    t.name = xstrdup(e->name);
    t.source_path = xstrdup(src_path);

    {
        size_t i;
        for (i = 0; i < e->extra_args.count; i++) {
            str_vec_push(&t.options, e->extra_args.items[i]);
        }
    }
    apply_label_options(&e->labels, &t.options);
    if (e->include_path && e->include_path[0] != '\0') {
        char inc[PATH_MAX_LOCAL];
        if (snprintf(inc, sizeof(inc), "-I%s/%s", integration_dir, e->include_path) < (int)sizeof(inc)) {
            str_vec_push(&t.options, inc);
        }
    }

    test_vec_push(out, &t);
}

static void collect_tests_from_cmake(const char *cmake_path, const char *integration_dir, test_vec_t *out) {
    char *text = read_text_file(cmake_path);
    char *clean;
    size_t i = 0;
    size_t n;

    if (!text) die("failed to read %s", cmake_path);
    clean = strip_comments(text);
    free(text);
    n = strlen(clean);

    while (i + 3 < n) {
        if (clean[i] == 'R' && clean[i + 1] == 'U' && clean[i + 2] == 'N') {
            size_t j = i + 3;
            while (j < n && isspace((unsigned char)clean[j])) j++;
            if (j < n && clean[j] == '(') {
                size_t start = j + 1;
                size_t k = start;
                int depth = 1;
                while (k < n && depth > 0) {
                    if (clean[k] == '(') depth++;
                    else if (clean[k] == ')') depth--;
                    k++;
                }
                if (depth == 0) {
                    run_entry_t e;
                    char *block = substr_dup(clean, start, k - 1);
                    e = parse_run_block(block);
                    add_test_from_entry(&e, integration_dir, out);
                    run_entry_free(&e);
                    free(block);
                    i = k;
                    continue;
                }
            }
        }
        i++;
    }

    free(clean);
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t w = 0;
    while (w < len) {
        ssize_t n = write(fd, p + w, len - w);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        w += (size_t)n;
    }
    return 0;
}

static char *read_fd_all(int fd) {
    char buf[4096];
    char *out = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            while (ncap < len + (size_t)n + 1) ncap *= 2;
            out = (char *)realloc(out, ncap);
            if (!out) die("out of memory");
            cap = ncap;
        }
        memcpy(out + len, buf, (size_t)n);
        len += (size_t)n;
    }
    if (!out) {
        out = (char *)malloc(1);
        if (!out) die("out of memory");
        out[0] = '\0';
        return out;
    }
    out[len] = '\0';
    return out;
}

static int make_temp_file(char *path_out, size_t cap, const char *prefix) {
    int fd;
    if (snprintf(path_out, cap, "%s_XXXXXX", prefix) >= (int)cap) return -1;
    fd = mkstemp(path_out);
    return fd;
}

static int run_cmd_capture(const char *const *argv,
                           int timeout_sec,
                           const char *stdout_file,
                           const char *runtime_dir,
                           cmd_result_t *out) {
    char out_tmp[PATH_MAX_LOCAL];
    char err_tmp[PATH_MAX_LOCAL];
    int out_fd = -1;
    int err_fd = -1;
    pid_t pid;
    int status = 0;
    bool timed_out = false;
    double t0 = now_ms();

    memset(out, 0, sizeof(*out));
    out->rc = -1;

    if (stdout_file) {
        out_fd = open(stdout_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out_fd < 0) return -1;
        out_tmp[0] = '\0';
    } else {
        if (make_temp_file(out_tmp, sizeof(out_tmp), "/tmp/liric_cmd_out") < 0) return -1;
        out_fd = open(out_tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (out_fd < 0) return -1;
    }

    if (make_temp_file(err_tmp, sizeof(err_tmp), "/tmp/liric_cmd_err") < 0) {
        close(out_fd);
        return -1;
    }
    err_fd = open(err_tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (err_fd < 0) {
        close(out_fd);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(out_fd);
        close(err_fd);
        return -1;
    }

    if (pid == 0) {
        if (runtime_dir && runtime_dir[0] != '\0') {
            setenv("DYLD_LIBRARY_PATH", runtime_dir, 1);
            setenv("LD_LIBRARY_PATH", runtime_dir, 1);
        }
        dup2(out_fd, STDOUT_FILENO);
        dup2(err_fd, STDERR_FILENO);
        close(out_fd);
        close(err_fd);
        execvp(argv[0], (char *const *)argv);
        {
            const char *msg = "execvp failed\n";
            write_all(STDERR_FILENO, msg, strlen(msg));
        }
        _exit(127);
    }

    close(out_fd);
    close(err_fd);

    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((now_ms() - t0) > (double)(timeout_sec * 1000)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(10000);
    }

    out->wall_ms = now_ms() - t0;
    if (timed_out) {
        out->rc = -99;
    } else if (WIFEXITED(status)) {
        out->rc = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out->rc = -WTERMSIG(status);
    } else {
        out->rc = -1;
    }

    if (stdout_file) {
        out->out = xstrdup("");
    } else {
        int fd = open(out_tmp, O_RDONLY);
        if (fd >= 0) {
            out->out = read_fd_all(fd);
            close(fd);
        } else {
            out->out = xstrdup("");
        }
        unlink(out_tmp);
    }

    {
        int fd = open(err_tmp, O_RDONLY);
        if (fd >= 0) {
            out->err = read_fd_all(fd);
            close(fd);
        } else {
            out->err = xstrdup("");
        }
        unlink(err_tmp);
    }

    return 0;
}

static void cmd_result_free(cmd_result_t *r) {
    free(r->out);
    free(r->err);
    r->out = NULL;
    r->err = NULL;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static double pct_of_vec(const dbl_vec_t *v, double p) {
    double *tmp;
    double k;
    size_t f, c;
    double frac;
    if (v->count == 0) return 0.0;
    tmp = (double *)malloc(v->count * sizeof(double));
    if (!tmp) die("out of memory");
    memcpy(tmp, v->items, v->count * sizeof(double));
    qsort(tmp, v->count, sizeof(double), cmp_double);

    if (v->count == 1) {
        double only = tmp[0];
        free(tmp);
        return only;
    }

    k = ((double)(v->count - 1) * p) / 100.0;
    f = (size_t)k;
    c = f + 1;
    if (c >= v->count) c = v->count - 1;
    frac = k - (double)f;

    {
        double out = tmp[f] + frac * (tmp[c] - tmp[f]);
        free(tmp);
        return out;
    }
}

static double median_of_vec(const dbl_vec_t *v) {
    return pct_of_vec(v, 50.0);
}

static char *normalize_output(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    size_t i = 0;
    size_t w = 0;
    if (!out) die("out of memory");

    while (i < n) {
        size_t line_start = i;
        size_t line_end = i;
        while (line_end < n && s[line_end] != '\n') line_end++;
        while (line_end > line_start && (s[line_end - 1] == ' ' || s[line_end - 1] == '\t' || s[line_end - 1] == '\r')) {
            line_end--;
        }
        if (line_end > line_start) {
            memcpy(out + w, s + line_start, line_end - line_start);
            w += line_end - line_start;
        }
        if (line_end < n || (line_start < n && s[line_start] == '\n')) {
            out[w++] = '\n';
        }
        if (line_end < n && s[line_end] == '\n') i = line_end + 1;
        else i = (line_end == line_start) ? i + 1 : line_end;
    }

    while (w > 0 && out[w - 1] == '\n') w--;
    out[w] = '\0';
    return out;
}

static bool parse_timing(const char *stderr_text, probe_timing_t *t) {
    const char *p = strstr(stderr_text, "TIMING ");
    char *line;
    char *tok;
    memset(t, 0, sizeof(*t));
    if (!p) return false;

    {
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        line = substr_dup(p, 0, (size_t)(eol - p));
    }

    tok = strtok(line, " ");
    while (tok) {
        char *eq = strchr(tok, '=');
        if (eq) {
            double val;
            *eq = '\0';
            val = strtod(eq + 1, NULL);
            if (strcmp(tok, "read_us") == 0) t->read_us = val;
            else if (strcmp(tok, "parse_us") == 0) t->parse_us = val;
            else if (strcmp(tok, "jit_create_us") == 0) t->jit_create_us = val;
            else if (strcmp(tok, "load_lib_us") == 0) t->load_lib_us = val;
            else if (strcmp(tok, "compile_us") == 0) t->compile_us = val;
            else if (strcmp(tok, "run_us") == 0) t->run_us = val;
            else if (strcmp(tok, "total_us") == 0) t->total_us = val;
        }
        tok = strtok(NULL, " ");
    }

    t->have = true;
    free(line);
    return true;
}

static char **build_argv_lfortran_compile(const bench_test_t *t,
                                          const char *lfortran,
                                          const char *bin_path) {
    size_t n = 0;
    char **argv;
    size_t i;

    n = 1 + 1 + t->options.count + 1 + 1 + 1 + 1;
    argv = (char **)calloc(n, sizeof(char *));
    if (!argv) die("out of memory");

    n = 0;
    argv[n++] = (char *)lfortran;
    argv[n++] = (char *)"--no-color";
    for (i = 0; i < t->options.count; i++) argv[n++] = t->options.items[i];
    argv[n++] = t->source_path;
    argv[n++] = (char *)"-o";
    argv[n++] = (char *)bin_path;
    argv[n++] = NULL;
    return argv;
}

static char **build_argv_lfortran_emit(const bench_test_t *t,
                                       const char *lfortran) {
    size_t n = 0;
    char **argv;
    size_t i;

    n = 1 + 1 + 1 + t->options.count + 1 + 1;
    argv = (char **)calloc(n, sizeof(char *));
    if (!argv) die("out of memory");

    n = 0;
    argv[n++] = (char *)lfortran;
    argv[n++] = (char *)"--no-color";
    argv[n++] = (char *)"--show-llvm";
    for (i = 0; i < t->options.count; i++) argv[n++] = t->options.items[i];
    argv[n++] = t->source_path;
    argv[n++] = NULL;
    return argv;
}

static char **build_argv_probe(const char *probe,
                               const char *runtime,
                               const char *ll_path) {
    char **argv = (char **)calloc(8, sizeof(char *));
    if (!argv) die("out of memory");
    argv[0] = (char *)probe;
    argv[1] = (char *)"--timing";
    argv[2] = (char *)"--sig";
    argv[3] = (char *)"i32_argc_argv";
    argv[4] = (char *)"--load-lib";
    argv[5] = (char *)runtime;
    argv[6] = (char *)ll_path;
    argv[7] = NULL;
    return argv;
}

static char **build_argv_lli(const char *lli,
                             const char *runtime,
                             const char *ll_path) {
    char **argv = (char **)calloc(6, sizeof(char *));
    if (!argv) die("out of memory");
    argv[0] = (char *)lli;
    argv[1] = (char *)"-O0";
    argv[2] = (char *)"--dlopen";
    argv[3] = (char *)runtime;
    argv[4] = (char *)ll_path;
    argv[5] = NULL;
    return argv;
}

static bool run_one_test(const cfg_t *cfg,
                         const bench_test_t *t,
                         const char *ll_dir,
                         const char *bin_dir,
                         bench_row_t *row) {
    char ll_path[PATH_MAX_LOCAL];
    char bin_path[PATH_MAX_LOCAL];
    dbl_vec_t exe_compile = {0}, exe_run = {0}, exe_wall = {0}, exe_non_parse = {0};
    dbl_vec_t api_emit = {0}, api_wall = {0}, api_parse = {0}, api_compile = {0}, api_run = {0}, api_non_parse = {0};
    dbl_vec_t llj_wall = {0}, llj_parse = {0}, llj_compile = {0}, llj_run = {0}, llj_non_parse = {0};
    dbl_vec_t lli_wall = {0};
    bool api_jit_match = true;
    bool ll_jit_match = true;
    bool ll_lli_match = true;
    int it;
    char *ref_norm = NULL;
    bool ok = true;

    memset(row, 0, sizeof(*row));
    row->name = xstrdup(t->name);

    if (snprintf(ll_path, sizeof(ll_path), "%s/%s.ll", ll_dir, t->name) >= (int)sizeof(ll_path)) {
        die("path too long for ll file");
    }
    if (snprintf(bin_path, sizeof(bin_path), "%s/%s", bin_dir, t->name) >= (int)sizeof(bin_path)) {
        die("path too long for bin file");
    }

    row->api_exe_ok = true;
    row->api_jit_ok = true;
    row->ll_jit_ok = true;
    row->ll_lli_ok = true;

    for (it = 0; it < cfg->iters; it++) {
        cmd_result_t c_compile, c_run, c_emit, c_api_jit, c_ll_jit, c_lli;
        probe_timing_t tim_api, tim_llj;
        char **argv_compile = NULL;
        char **argv_run = NULL;
        char **argv_emit = NULL;
        char **argv_probe_api = NULL;
        char **argv_probe_llj = NULL;
        char **argv_lli = NULL;
        char *run_norm = NULL;
        char *jit_norm = NULL;
        char *llj_norm = NULL;
        char *lli_norm = NULL;
        char *runtime_dir;

        memset(&c_compile, 0, sizeof(c_compile));
        memset(&c_run, 0, sizeof(c_run));
        memset(&c_emit, 0, sizeof(c_emit));
        memset(&c_api_jit, 0, sizeof(c_api_jit));
        memset(&c_ll_jit, 0, sizeof(c_ll_jit));
        memset(&c_lli, 0, sizeof(c_lli));

        argv_compile = build_argv_lfortran_compile(t, cfg->lfortran, bin_path);
        if (run_cmd_capture((const char *const *)argv_compile, cfg->timeout_sec, NULL, NULL, &c_compile) != 0) {
            ok = false;
            free(argv_compile);
            break;
        }
        free(argv_compile);

        if (c_compile.rc != 0) {
            row->api_exe_ok = false;
            row->api_jit_ok = false;
            row->ll_jit_ok = false;
            row->ll_lli_ok = false;
            cmd_result_free(&c_compile);
            ok = false;
            break;
        }

        argv_run = (char **)calloc(2, sizeof(char *));
        if (!argv_run) die("out of memory");
        argv_run[0] = bin_path;
        argv_run[1] = NULL;
        if (run_cmd_capture((const char *const *)argv_run, cfg->timeout_sec, NULL, NULL, &c_run) != 0) {
            ok = false;
            free(argv_run);
            cmd_result_free(&c_compile);
            break;
        }
        free(argv_run);

        if (c_run.rc < 0) {
            row->api_exe_ok = false;
            row->api_jit_ok = false;
            row->ll_jit_ok = false;
            row->ll_lli_ok = false;
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            ok = false;
            break;
        }

        run_norm = normalize_output(c_run.out);
        if (it == 0) {
            ref_norm = xstrdup(run_norm);
        }

        dbl_vec_push(&exe_compile, c_compile.wall_ms);
        dbl_vec_push(&exe_run, c_run.wall_ms);
        dbl_vec_push(&exe_wall, c_compile.wall_ms + c_run.wall_ms);
        dbl_vec_push(&exe_non_parse, c_compile.wall_ms + c_run.wall_ms);

        argv_emit = build_argv_lfortran_emit(t, cfg->lfortran);
        if (run_cmd_capture((const char *const *)argv_emit, cfg->timeout_sec, ll_path, NULL, &c_emit) != 0) {
            ok = false;
            free(argv_emit);
            free(run_norm);
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            break;
        }
        free(argv_emit);
        if (c_emit.rc != 0) {
            row->api_jit_ok = false;
            row->ll_jit_ok = false;
            row->ll_lli_ok = false;
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            free(run_norm);
            ok = false;
            break;
        }

        argv_probe_api = build_argv_probe(cfg->probe_runner, cfg->runtime_lib, ll_path);
        if (run_cmd_capture((const char *const *)argv_probe_api, cfg->timeout_sec, NULL, NULL, &c_api_jit) != 0) {
            ok = false;
            free(argv_probe_api);
            free(run_norm);
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            break;
        }
        free(argv_probe_api);

        if (c_api_jit.rc < 0) {
            row->api_jit_ok = false;
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            cmd_result_free(&c_api_jit);
            free(run_norm);
            ok = false;
            break;
        }
        parse_timing(c_api_jit.err, &tim_api);
        jit_norm = normalize_output(c_api_jit.out);
        if (strcmp(jit_norm, run_norm) != 0 || c_api_jit.rc != c_run.rc) {
            api_jit_match = false;
        }

        dbl_vec_push(&api_emit, c_emit.wall_ms);
        dbl_vec_push(&api_wall, c_emit.wall_ms + c_api_jit.wall_ms);
        if (tim_api.have) {
            dbl_vec_push(&api_parse, tim_api.parse_us / 1000.0);
            dbl_vec_push(&api_compile, tim_api.compile_us / 1000.0);
            dbl_vec_push(&api_run, tim_api.run_us / 1000.0);
            dbl_vec_push(&api_non_parse, (tim_api.compile_us + tim_api.run_us) / 1000.0);
        }

        argv_probe_llj = build_argv_probe(cfg->probe_runner, cfg->runtime_lib, ll_path);
        if (run_cmd_capture((const char *const *)argv_probe_llj, cfg->timeout_sec, NULL, NULL, &c_ll_jit) != 0) {
            ok = false;
            free(argv_probe_llj);
            free(run_norm);
            free(jit_norm);
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            cmd_result_free(&c_api_jit);
            break;
        }
        free(argv_probe_llj);

        if (c_ll_jit.rc < 0) {
            row->ll_jit_ok = false;
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            cmd_result_free(&c_api_jit);
            cmd_result_free(&c_ll_jit);
            free(run_norm);
            free(jit_norm);
            ok = false;
            break;
        }

        parse_timing(c_ll_jit.err, &tim_llj);
        llj_norm = normalize_output(c_ll_jit.out);
        if (strcmp(llj_norm, run_norm) != 0 || c_ll_jit.rc != c_run.rc) {
            ll_jit_match = false;
        }

        dbl_vec_push(&llj_wall, c_ll_jit.wall_ms);
        if (tim_llj.have) {
            dbl_vec_push(&llj_parse, tim_llj.parse_us / 1000.0);
            dbl_vec_push(&llj_compile, tim_llj.compile_us / 1000.0);
            dbl_vec_push(&llj_run, tim_llj.run_us / 1000.0);
            dbl_vec_push(&llj_non_parse, (tim_llj.compile_us + tim_llj.run_us) / 1000.0);
        }

        argv_lli = build_argv_lli(cfg->lli, cfg->runtime_lib, ll_path);
        runtime_dir = dirname_copy(cfg->runtime_lib);
        if (run_cmd_capture((const char *const *)argv_lli, cfg->timeout_sec, NULL, runtime_dir, &c_lli) != 0) {
            ok = false;
            free(runtime_dir);
            free(argv_lli);
            free(run_norm);
            free(jit_norm);
            free(llj_norm);
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            cmd_result_free(&c_api_jit);
            cmd_result_free(&c_ll_jit);
            break;
        }
        free(runtime_dir);
        free(argv_lli);

        if (c_lli.rc < 0) {
            row->ll_lli_ok = false;
            cmd_result_free(&c_compile);
            cmd_result_free(&c_run);
            cmd_result_free(&c_emit);
            cmd_result_free(&c_api_jit);
            cmd_result_free(&c_ll_jit);
            cmd_result_free(&c_lli);
            free(run_norm);
            free(jit_norm);
            free(llj_norm);
            ok = false;
            break;
        }

        lli_norm = normalize_output(c_lli.out);
        if (strcmp(lli_norm, run_norm) != 0 || c_lli.rc != c_run.rc) {
            ll_lli_match = false;
        }
        dbl_vec_push(&lli_wall, c_lli.wall_ms);

        cmd_result_free(&c_compile);
        cmd_result_free(&c_run);
        cmd_result_free(&c_emit);
        cmd_result_free(&c_api_jit);
        cmd_result_free(&c_ll_jit);
        cmd_result_free(&c_lli);

        free(run_norm);
        free(jit_norm);
        free(llj_norm);
        free(lli_norm);
    }

    if (!ok) {
        free(ref_norm);
        dbl_vec_free(&exe_compile);
        dbl_vec_free(&exe_run);
        dbl_vec_free(&exe_wall);
        dbl_vec_free(&exe_non_parse);
        dbl_vec_free(&api_emit);
        dbl_vec_free(&api_wall);
        dbl_vec_free(&api_parse);
        dbl_vec_free(&api_compile);
        dbl_vec_free(&api_run);
        dbl_vec_free(&api_non_parse);
        dbl_vec_free(&llj_wall);
        dbl_vec_free(&llj_parse);
        dbl_vec_free(&llj_compile);
        dbl_vec_free(&llj_run);
        dbl_vec_free(&llj_non_parse);
        dbl_vec_free(&lli_wall);
        row->api_jit_match = false;
        row->ll_jit_match = false;
        row->ll_lli_match = false;
        return false;
    }

    row->api_jit_match = row->api_jit_ok && api_jit_match;
    row->ll_jit_match = row->ll_jit_ok && ll_jit_match;
    row->ll_lli_match = row->ll_lli_ok && ll_lli_match;

    row->api_exe_compile_ms = median_of_vec(&exe_compile);
    row->api_exe_run_ms = median_of_vec(&exe_run);
    row->api_exe_wall_ms = median_of_vec(&exe_wall);
    row->api_exe_non_parse_ms = median_of_vec(&exe_non_parse);

    row->api_jit_emit_ms = median_of_vec(&api_emit);
    row->api_jit_wall_ms = median_of_vec(&api_wall);
    row->api_jit_parse_ms = median_of_vec(&api_parse);
    row->api_jit_compile_ms = median_of_vec(&api_compile);
    row->api_jit_run_ms = median_of_vec(&api_run);
    row->api_jit_non_parse_ms = median_of_vec(&api_non_parse);

    row->ll_jit_wall_ms = median_of_vec(&llj_wall);
    row->ll_jit_parse_ms = median_of_vec(&llj_parse);
    row->ll_jit_compile_ms = median_of_vec(&llj_compile);
    row->ll_jit_run_ms = median_of_vec(&llj_run);
    row->ll_jit_non_parse_ms = median_of_vec(&llj_non_parse);

    row->ll_lli_wall_ms = median_of_vec(&lli_wall);

    free(ref_norm);
    dbl_vec_free(&exe_compile);
    dbl_vec_free(&exe_run);
    dbl_vec_free(&exe_wall);
    dbl_vec_free(&exe_non_parse);
    dbl_vec_free(&api_emit);
    dbl_vec_free(&api_wall);
    dbl_vec_free(&api_parse);
    dbl_vec_free(&api_compile);
    dbl_vec_free(&api_run);
    dbl_vec_free(&api_non_parse);
    dbl_vec_free(&llj_wall);
    dbl_vec_free(&llj_parse);
    dbl_vec_free(&llj_compile);
    dbl_vec_free(&llj_run);
    dbl_vec_free(&llj_non_parse);
    dbl_vec_free(&lli_wall);

    return true;
}

static void json_escape(FILE *f, const char *s) {
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', f);
            fputc(c, f);
        } else if (c == '\n') {
            fputs("\\n", f);
        } else if (c == '\r') {
            fputs("\\r", f);
        } else if (c == '\t') {
            fputs("\\t", f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", (unsigned)c);
        } else {
            fputc(c, f);
        }
        s++;
    }
}

static void write_row_json(FILE *f, const bench_row_t *r) {
    fprintf(f, "{\"name\":\"");
    json_escape(f, r->name);
    fprintf(f, "\"");

    fprintf(f, ",\"api_exe_ok\":%s", r->api_exe_ok ? "true" : "false");
    fprintf(f, ",\"api_jit_ok\":%s", r->api_jit_ok ? "true" : "false");
    fprintf(f, ",\"ll_jit_ok\":%s", r->ll_jit_ok ? "true" : "false");
    fprintf(f, ",\"ll_lli_ok\":%s", r->ll_lli_ok ? "true" : "false");
    fprintf(f, ",\"api_jit_match\":%s", r->api_jit_match ? "true" : "false");
    fprintf(f, ",\"ll_jit_match\":%s", r->ll_jit_match ? "true" : "false");
    fprintf(f, ",\"ll_lli_match\":%s", r->ll_lli_match ? "true" : "false");

    fprintf(f, ",\"api_exe_compile_ms\":%.6f", r->api_exe_compile_ms);
    fprintf(f, ",\"api_exe_run_ms\":%.6f", r->api_exe_run_ms);
    fprintf(f, ",\"api_exe_wall_ms\":%.6f", r->api_exe_wall_ms);
    fprintf(f, ",\"api_exe_non_parse_ms\":%.6f", r->api_exe_non_parse_ms);

    fprintf(f, ",\"api_jit_emit_ms\":%.6f", r->api_jit_emit_ms);
    fprintf(f, ",\"api_jit_wall_ms\":%.6f", r->api_jit_wall_ms);
    fprintf(f, ",\"api_jit_parse_ms\":%.6f", r->api_jit_parse_ms);
    fprintf(f, ",\"api_jit_compile_ms\":%.6f", r->api_jit_compile_ms);
    fprintf(f, ",\"api_jit_run_ms\":%.6f", r->api_jit_run_ms);
    fprintf(f, ",\"api_jit_non_parse_ms\":%.6f", r->api_jit_non_parse_ms);

    fprintf(f, ",\"ll_jit_wall_ms\":%.6f", r->ll_jit_wall_ms);
    fprintf(f, ",\"ll_jit_parse_ms\":%.6f", r->ll_jit_parse_ms);
    fprintf(f, ",\"ll_jit_compile_ms\":%.6f", r->ll_jit_compile_ms);
    fprintf(f, ",\"ll_jit_run_ms\":%.6f", r->ll_jit_run_ms);
    fprintf(f, ",\"ll_jit_non_parse_ms\":%.6f", r->ll_jit_non_parse_ms);

    fprintf(f, ",\"ll_lli_wall_ms\":%.6f", r->ll_lli_wall_ms);
    fprintf(f, "}\n");
}

static void parse_args(int argc, char **argv, cfg_t *cfg) {
    int i;
    memset(cfg, 0, sizeof(*cfg));
    cfg->iters = 3;
    cfg->timeout_sec = 15;
    cfg->limit = 0;
    cfg->bench_dir = "/tmp/liric_bench";
    cfg->integration_cmake = "../lfortran/integration_tests/CMakeLists.txt";
    cfg->integration_dir = NULL;
    cfg->lfortran = "../lfortran/build/src/bin/lfortran";
    cfg->probe_runner = "build/liric_probe_runner";
    cfg->runtime_lib = "../lfortran/build/src/runtime/liblfortran_runtime.dylib";
    cfg->lli = "/opt/homebrew/opt/llvm/bin/lli";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            cfg->iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg->timeout_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            cfg->limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            cfg->bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--integration-cmake") == 0 && i + 1 < argc) {
            cfg->integration_cmake = argv[++i];
        } else if (strcmp(argv[i], "--integration-dir") == 0 && i + 1 < argc) {
            cfg->integration_dir = argv[++i];
        } else if (strcmp(argv[i], "--lfortran") == 0 && i + 1 < argc) {
            cfg->lfortran = argv[++i];
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg->probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            cfg->runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--lli") == 0 && i + 1 < argc) {
            cfg->lli = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: bench_matrix [options]\n");
            printf("  --iters N\n");
            printf("  --timeout SEC\n");
            printf("  --limit N\n");
            printf("  --bench-dir PATH\n");
            printf("  --integration-cmake PATH\n");
            printf("  --integration-dir PATH\n");
            printf("  --lfortran PATH\n");
            printf("  --probe-runner PATH\n");
            printf("  --runtime-lib PATH\n");
            printf("  --lli PATH\n");
            exit(0);
        } else {
            die("unknown arg: %s", argv[i]);
        }
    }

    if (cfg->iters <= 0) die("--iters must be > 0");
    if (cfg->timeout_sec <= 0) die("--timeout must be > 0");

    if (!cfg->integration_dir) {
        static char *auto_dir = NULL;
        auto_dir = dirname_copy(cfg->integration_cmake);
        cfg->integration_dir = auto_dir;
    }

    if (!file_exists(cfg->runtime_lib)) {
        if (has_suffix(cfg->runtime_lib, ".dylib")) {
            static char alt_runtime[PATH_MAX_LOCAL];
            size_t n = strlen(cfg->runtime_lib);
            if (n >= 6 && n + 1 < sizeof(alt_runtime)) {
                snprintf(alt_runtime, sizeof(alt_runtime), "%.*s.so", (int)(n - 6), cfg->runtime_lib);
                if (file_exists(alt_runtime)) cfg->runtime_lib = alt_runtime;
            }
        }
    }

    if (!file_exists(cfg->lli)) {
        cfg->lli = "lli";
    }

    if (!file_exists(cfg->lfortran)) die("lfortran not found: %s", cfg->lfortran);
    if (!file_exists(cfg->probe_runner)) die("probe runner not found: %s", cfg->probe_runner);
    if (!file_exists(cfg->runtime_lib)) die("runtime lib not found: %s", cfg->runtime_lib);
    if (!file_exists(cfg->integration_cmake)) die("integration CMakeLists not found: %s", cfg->integration_cmake);
}

static void print_lane_summary(const char *name,
                               const dbl_vec_t *wall,
                               const dbl_vec_t *compile,
                               const dbl_vec_t *run,
                               const dbl_vec_t *parse,
                               const dbl_vec_t *non_parse) {
    printf("  %-10s wall=%8.3fms", name, median_of_vec(wall));
    if (compile && compile->count) printf(" compile=%8.3fms", median_of_vec(compile));
    else printf(" compile=%8s", "n/a");
    if (run && run->count) printf(" run=%8.3fms", median_of_vec(run));
    else printf(" run=%8s", "n/a");
    if (parse && parse->count) printf(" parse=%8.3fms", median_of_vec(parse));
    else printf(" parse=%8s", "n/a");
    if (non_parse && non_parse->count) printf(" non_parse=%8.3fms", median_of_vec(non_parse));
    else printf(" non_parse=%8s", "n/a");
    printf("\n");
}

static void write_summary_md(const char *path,
                             const cfg_t *cfg,
                             const row_vec_t *rows,
                             const dbl_vec_t *api_exe_wall,
                             const dbl_vec_t *api_exe_compile,
                             const dbl_vec_t *api_exe_run,
                             const dbl_vec_t *api_exe_non_parse,
                             const dbl_vec_t *api_jit_wall,
                             const dbl_vec_t *api_jit_compile,
                             const dbl_vec_t *api_jit_run,
                             const dbl_vec_t *api_jit_parse,
                             const dbl_vec_t *api_jit_non_parse,
                             const dbl_vec_t *ll_jit_wall,
                             const dbl_vec_t *ll_jit_compile,
                             const dbl_vec_t *ll_jit_run,
                             const dbl_vec_t *ll_jit_parse,
                             const dbl_vec_t *ll_jit_non_parse,
                             const dbl_vec_t *ll_lli_wall,
                             const dbl_vec_t *api_cmp_exe_wall,
                             const dbl_vec_t *api_cmp_jit_wall,
                             const dbl_vec_t *api_cmp_exe_non_parse,
                             const dbl_vec_t *api_cmp_jit_non_parse,
                             const dbl_vec_t *ll_cmp_lli_wall,
                             const dbl_vec_t *ll_cmp_jit_wall) {
    FILE *f = fopen(path, "wb");
    if (!f) die("failed to write summary: %s", path);

    fprintf(f, "# Benchmark Matrix\n\n");
    fprintf(f, "- Iterations: %d\n", cfg->iters);
    fprintf(f, "- Timeout: %d sec\n", cfg->timeout_sec);
    fprintf(f, "- Tests processed: %zu\n\n", rows->count);

    fprintf(f, "## Lane/Mode Matrix\n\n");
    fprintf(f, "| Lane | Source | Engine | wall | compile_only | run_only | parse_only | non_parse |\n");
    fprintf(f, "|------|--------|--------|------|--------------|----------|------------|-----------|\n");
    fprintf(f, "| api_exe | .f90 | lfortran native exe | yes | yes | yes | no | yes (=compile+run) |\n");
    fprintf(f, "| api_jit | .f90 -> .ll | liric JIT | yes | yes | yes | yes | yes (=compile+run) |\n");
    fprintf(f, "| ll_jit | .ll | liric JIT | yes | yes | yes | yes | yes (=compile+run) |\n");
    fprintf(f, "| ll_lli | .ll | lli -O0 | yes | no | no | no | no |\n\n");

    fprintf(f, "## Lane Medians (ms)\n\n");
    fprintf(f, "| Lane | wall | compile_only | run_only | parse_only | non_parse |\n");
    fprintf(f, "|------|-----:|-------------:|---------:|-----------:|----------:|\n");
    fprintf(f, "| api_exe | %.3f | %.3f | %.3f | n/a | %.3f |\n",
            median_of_vec(api_exe_wall), median_of_vec(api_exe_compile),
            median_of_vec(api_exe_run), median_of_vec(api_exe_non_parse));
    fprintf(f, "| api_jit | %.3f | %.3f | %.3f | %.3f | %.3f |\n",
            median_of_vec(api_jit_wall), median_of_vec(api_jit_compile),
            median_of_vec(api_jit_run), median_of_vec(api_jit_parse),
            median_of_vec(api_jit_non_parse));
    fprintf(f, "| ll_jit | %.3f | %.3f | %.3f | %.3f | %.3f |\n",
            median_of_vec(ll_jit_wall), median_of_vec(ll_jit_compile),
            median_of_vec(ll_jit_run), median_of_vec(ll_jit_parse),
            median_of_vec(ll_jit_non_parse));
    fprintf(f, "| ll_lli | %.3f | n/a | n/a | n/a | n/a |\n\n",
            median_of_vec(ll_lli_wall));

    fprintf(f, "## Comparison Lanes\n\n");
    if (api_cmp_exe_wall->count && api_cmp_jit_wall->count) {
        double s_wall = median_of_vec(api_cmp_exe_wall) / median_of_vec(api_cmp_jit_wall);
        double s_np = median_of_vec(api_cmp_exe_non_parse) / median_of_vec(api_cmp_jit_non_parse);
        fprintf(f, "- api_e2e wall speedup (exe/jit): %.3fx\n", s_wall);
        fprintf(f, "- api_non_parse speedup (exe non_parse / jit non_parse): %.3fx\n", s_np);
    } else {
        fprintf(f, "- api_e2e: n/a (no matched tests)\n");
    }

    if (ll_cmp_lli_wall->count && ll_cmp_jit_wall->count) {
        double s_ll = median_of_vec(ll_cmp_lli_wall) / median_of_vec(ll_cmp_jit_wall);
        fprintf(f, "- ll_e2e wall speedup (lli/jit): %.3fx\n", s_ll);
    } else {
        fprintf(f, "- ll_e2e: n/a (no matched tests)\n");
    }

    fprintf(f, "\n## Compatibility Counts\n\n");
    {
        size_t i;
        size_t api_match = 0, ll_match = 0;
        for (i = 0; i < rows->count; i++) {
            const bench_row_t *r = &rows->items[i];
            if (r->api_exe_ok && r->api_jit_ok && r->api_jit_match) api_match++;
            if (r->api_exe_ok && r->ll_jit_ok && r->ll_jit_match && r->ll_lli_ok && r->ll_lli_match) ll_match++;
        }
        fprintf(f, "- api matched tests: %zu\n", api_match);
        fprintf(f, "- ll matched tests: %zu\n", ll_match);
    }

    fclose(f);
}

int main(int argc, char **argv) {
    cfg_t cfg;
    test_vec_t tests = {0};
    row_vec_t rows = {0};
    char ll_dir[PATH_MAX_LOCAL];
    char bin_dir[PATH_MAX_LOCAL];
    char rows_jsonl[PATH_MAX_LOCAL];
    char compat_api[PATH_MAX_LOCAL];
    char compat_ll[PATH_MAX_LOCAL];
    char summary_md[PATH_MAX_LOCAL];
    FILE *jf;
    FILE *fa;
    FILE *fl;
    size_t i;

    dbl_vec_t api_exe_wall = {0}, api_exe_compile = {0}, api_exe_run = {0}, api_exe_non_parse = {0};
    dbl_vec_t api_jit_wall = {0}, api_jit_compile = {0}, api_jit_run = {0}, api_jit_parse = {0}, api_jit_non_parse = {0};
    dbl_vec_t ll_jit_wall = {0}, ll_jit_compile = {0}, ll_jit_run = {0}, ll_jit_parse = {0}, ll_jit_non_parse = {0};
    dbl_vec_t ll_lli_wall = {0};

    dbl_vec_t api_cmp_exe_wall = {0}, api_cmp_jit_wall = {0};
    dbl_vec_t api_cmp_exe_non_parse = {0}, api_cmp_jit_non_parse = {0};
    dbl_vec_t ll_cmp_lli_wall = {0}, ll_cmp_jit_wall = {0};

    parse_args(argc, argv, &cfg);

    mkdir_p(cfg.bench_dir);
    build_path(ll_dir, sizeof(ll_dir), cfg.bench_dir, "ll");
    build_path(bin_dir, sizeof(bin_dir), cfg.bench_dir, "bin");
    mkdir_p(ll_dir);
    mkdir_p(bin_dir);

    collect_tests_from_cmake(cfg.integration_cmake, cfg.integration_dir, &tests);
    if (cfg.limit > 0 && (size_t)cfg.limit < tests.count) {
        tests.count = (size_t)cfg.limit;
    }

    if (tests.count == 0) {
        die("no eligible tests found in %s", cfg.integration_cmake);
    }

    printf("Benchmarking %zu tests, %d iterations each\n", tests.count, cfg.iters);

    build_path(rows_jsonl, sizeof(rows_jsonl), cfg.bench_dir, "bench_matrix_rows.jsonl");
    build_path(compat_api, sizeof(compat_api), cfg.bench_dir, "compat_api.txt");
    build_path(compat_ll, sizeof(compat_ll), cfg.bench_dir, "compat_ll.txt");
    build_path(summary_md, sizeof(summary_md), cfg.bench_dir, "summary.md");

    jf = fopen(rows_jsonl, "wb");
    if (!jf) die("failed to open rows jsonl");
    fa = fopen(compat_api, "wb");
    if (!fa) die("failed to open compat_api");
    fl = fopen(compat_ll, "wb");
    if (!fl) die("failed to open compat_ll");

    for (i = 0; i < tests.count; i++) {
        bench_row_t row;
        bool ok = run_one_test(&cfg, &tests.items[i], ll_dir, bin_dir, &row);
        row_vec_push(&rows, &row);
        write_row_json(jf, &row);

        if (ok && row.api_exe_ok) {
            dbl_vec_push(&api_exe_wall, row.api_exe_wall_ms);
            dbl_vec_push(&api_exe_compile, row.api_exe_compile_ms);
            dbl_vec_push(&api_exe_run, row.api_exe_run_ms);
            dbl_vec_push(&api_exe_non_parse, row.api_exe_non_parse_ms);
        }
        if (ok && row.api_jit_ok) {
            dbl_vec_push(&api_jit_wall, row.api_jit_wall_ms);
            dbl_vec_push(&api_jit_compile, row.api_jit_compile_ms);
            dbl_vec_push(&api_jit_run, row.api_jit_run_ms);
            dbl_vec_push(&api_jit_parse, row.api_jit_parse_ms);
            dbl_vec_push(&api_jit_non_parse, row.api_jit_non_parse_ms);
        }
        if (ok && row.ll_jit_ok) {
            dbl_vec_push(&ll_jit_wall, row.ll_jit_wall_ms);
            dbl_vec_push(&ll_jit_compile, row.ll_jit_compile_ms);
            dbl_vec_push(&ll_jit_run, row.ll_jit_run_ms);
            dbl_vec_push(&ll_jit_parse, row.ll_jit_parse_ms);
            dbl_vec_push(&ll_jit_non_parse, row.ll_jit_non_parse_ms);
        }
        if (ok && row.ll_lli_ok) {
            dbl_vec_push(&ll_lli_wall, row.ll_lli_wall_ms);
        }

        if (ok && row.api_exe_ok && row.api_jit_ok && row.api_jit_match) {
            dbl_vec_push(&api_cmp_exe_wall, row.api_exe_wall_ms);
            dbl_vec_push(&api_cmp_jit_wall, row.api_jit_wall_ms);
            dbl_vec_push(&api_cmp_exe_non_parse, row.api_exe_non_parse_ms);
            dbl_vec_push(&api_cmp_jit_non_parse, row.api_jit_non_parse_ms);
            fprintf(fa, "%s\n", row.name);
        }

        if (ok && row.api_exe_ok && row.ll_jit_ok && row.ll_jit_match && row.ll_lli_ok && row.ll_lli_match) {
            dbl_vec_push(&ll_cmp_lli_wall, row.ll_lli_wall_ms);
            dbl_vec_push(&ll_cmp_jit_wall, row.ll_jit_wall_ms);
            fprintf(fl, "%s\n", row.name);
        }

        if ((i + 1) % 25 == 0 || i + 1 == tests.count) {
            printf("  %zu/%zu\n", i + 1, tests.count);
        }
    }

    fclose(jf);
    fclose(fa);
    fclose(fl);

    printf("\nLane medians:\n");
    print_lane_summary("api_exe", &api_exe_wall, &api_exe_compile, &api_exe_run, NULL, &api_exe_non_parse);
    print_lane_summary("api_jit", &api_jit_wall, &api_jit_compile, &api_jit_run, &api_jit_parse, &api_jit_non_parse);
    print_lane_summary("ll_jit", &ll_jit_wall, &ll_jit_compile, &ll_jit_run, &ll_jit_parse, &ll_jit_non_parse);
    print_lane_summary("ll_lli", &ll_lli_wall, NULL, NULL, NULL, NULL);

    if (api_cmp_exe_wall.count && api_cmp_jit_wall.count) {
        double api_wall_speedup = median_of_vec(&api_cmp_exe_wall) / median_of_vec(&api_cmp_jit_wall);
        double api_np_speedup = median_of_vec(&api_cmp_exe_non_parse) / median_of_vec(&api_cmp_jit_non_parse);
        printf("\napi_e2e wall speedup (exe/jit): %.3fx\n", api_wall_speedup);
        printf("api_non_parse speedup (exe/jit): %.3fx\n", api_np_speedup);
    } else {
        printf("\napi_e2e: n/a (no matched tests)\n");
    }

    if (ll_cmp_lli_wall.count && ll_cmp_jit_wall.count) {
        double ll_wall_speedup = median_of_vec(&ll_cmp_lli_wall) / median_of_vec(&ll_cmp_jit_wall);
        printf("ll_e2e wall speedup (lli/jit): %.3fx\n", ll_wall_speedup);
    } else {
        printf("ll_e2e: n/a (no matched tests)\n");
    }

    write_summary_md(summary_md, &cfg, &rows,
                     &api_exe_wall, &api_exe_compile, &api_exe_run, &api_exe_non_parse,
                     &api_jit_wall, &api_jit_compile, &api_jit_run, &api_jit_parse, &api_jit_non_parse,
                     &ll_jit_wall, &ll_jit_compile, &ll_jit_run, &ll_jit_parse, &ll_jit_non_parse,
                     &ll_lli_wall,
                     &api_cmp_exe_wall, &api_cmp_jit_wall, &api_cmp_exe_non_parse, &api_cmp_jit_non_parse,
                     &ll_cmp_lli_wall, &ll_cmp_jit_wall);

    printf("\nArtifacts:\n");
    printf("  %s\n", rows_jsonl);
    printf("  %s\n", compat_api);
    printf("  %s\n", compat_ll);
    printf("  %s\n", summary_md);

    test_vec_free(&tests);
    row_vec_free(&rows);

    dbl_vec_free(&api_exe_wall);
    dbl_vec_free(&api_exe_compile);
    dbl_vec_free(&api_exe_run);
    dbl_vec_free(&api_exe_non_parse);
    dbl_vec_free(&api_jit_wall);
    dbl_vec_free(&api_jit_compile);
    dbl_vec_free(&api_jit_run);
    dbl_vec_free(&api_jit_parse);
    dbl_vec_free(&api_jit_non_parse);
    dbl_vec_free(&ll_jit_wall);
    dbl_vec_free(&ll_jit_compile);
    dbl_vec_free(&ll_jit_run);
    dbl_vec_free(&ll_jit_parse);
    dbl_vec_free(&ll_jit_non_parse);
    dbl_vec_free(&ll_lli_wall);
    dbl_vec_free(&api_cmp_exe_wall);
    dbl_vec_free(&api_cmp_jit_wall);
    dbl_vec_free(&api_cmp_exe_non_parse);
    dbl_vec_free(&api_cmp_jit_non_parse);
    dbl_vec_free(&ll_cmp_lli_wall);
    dbl_vec_free(&ll_cmp_jit_wall);

    return 0;
}
