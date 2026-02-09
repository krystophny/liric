// API benchmark: lfortran+LLVM (compile+link+run) vs lfortran+liric (compile+JIT+run).
// Measures how much faster the liric backend is compared to the full LLVM pipeline.
// Two metrics: wall-clock (total subprocess) and internal (codegen-only timing).

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char **items;
    size_t n;
    size_t cap;
} strlist_t;

typedef struct {
    int rc;
    char *stdout_text;
    char *stderr_text;
    double elapsed_ms;
    int timed_out;
} cmd_result_t;

typedef struct {
    const char *lfortran;
    const char *lfortran_liric;
    const char *probe_runner;
    const char *runtime_lib;
    const char *test_dir;
    const char *bench_dir;
    const char *compat_list;
    const char *options_jsonl;
    int iters;
    int timeout_sec;
    int min_completed;
} cfg_t;

typedef struct {
    char *name;
    char *options;
} name_opt_t;

typedef struct {
    name_opt_t *items;
    size_t n;
    size_t cap;
} optlist_t;

typedef struct {
    char *name;
    double liric_wall_ms;
    double llvm_wall_ms;
    double liric_compile_ms;
    double liric_run_ms;
    double llvm_compile_ms;
    double llvm_run_ms;
} row_t;

typedef struct {
    row_t *items;
    size_t n;
    size_t cap;
} rowlist_t;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static volatile sig_atomic_t g_timeout_child = -1;
static volatile sig_atomic_t g_timeout_fired = 0;

static void timeout_kill_handler(int signo) {
    pid_t child;
    (void)signo;
    child = (pid_t)g_timeout_child;
    if (child > 0) {
        g_timeout_fired = 1;
        kill(child, SIGKILL);
    }
}

static void die(const char *msg, const char *path) {
    if (path) fprintf(stderr, "%s: %s\n", msg, path);
    else fprintf(stderr, "%s\n", msg);
    exit(1);
}

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void ensure_dir(const char *path) {
    if (is_dir(path)) return;
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        die("failed to create dir", path);
}

static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) die("out of memory", NULL);
    memcpy(p, s, n + 1);
    return p;
}

static char *to_abs_path(const char *path) {
    char cwd[PATH_MAX];
    size_t nc, np;
    char *out;
    if (!path) return NULL;
    if (path[0] == '/') return xstrdup(path);
    if (!getcwd(cwd, sizeof(cwd))) die("getcwd failed", NULL);
    nc = strlen(cwd);
    np = strlen(path);
    out = (char *)malloc(nc + 1 + np + 1);
    if (!out) die("out of memory", NULL);
    memcpy(out, cwd, nc);
    out[nc] = '/';
    memcpy(out + nc + 1, path, np + 1);
    return out;
}

static char *path_join2(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    int need = (na > 0 && a[na - 1] != '/');
    char *out = (char *)malloc(na + nb + (need ? 2 : 1));
    if (!out) die("out of memory", NULL);
    memcpy(out, a, na);
    if (need) out[na++] = '/';
    memcpy(out + na, b, nb);
    out[na + nb] = '\0';
    return out;
}

static char *read_all_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    size_t nread;
    char *buf;
    if (!f) return xstrdup("");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return xstrdup("");
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return xstrdup("");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return xstrdup("");
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) die("out of memory", NULL);
    nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

static int wait_with_timeout(pid_t pid, int timeout_sec, int *status_out) {
    int status;
    pid_t r;

    if (timeout_sec <= 0) {
        do {
            r = waitpid(pid, &status, 0);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            *status_out = 0;
            return -1;
        }
        *status_out = status;
        return 0;
    }

    {
        struct sigaction sa_new, sa_old;
        memset(&sa_new, 0, sizeof(sa_new));
        sa_new.sa_handler = timeout_kill_handler;
        sigemptyset(&sa_new.sa_mask);
        sa_new.sa_flags = 0;
        if (sigaction(SIGALRM, &sa_new, &sa_old) != 0) {
            *status_out = 0;
            return -1;
        }

        g_timeout_child = (sig_atomic_t)pid;
        g_timeout_fired = 0;
        alarm((unsigned int)timeout_sec);

        do {
            r = waitpid(pid, &status, 0);
        } while (r < 0 && errno == EINTR);

        alarm(0);
        g_timeout_child = -1;
        sigaction(SIGALRM, &sa_old, NULL);

        if (r < 0) {
            *status_out = 0;
            g_timeout_fired = 0;
            return -1;
        }
        *status_out = status;

        if (g_timeout_fired &&
            WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGKILL) {
            g_timeout_fired = 0;
            return 1;
        }
        g_timeout_fired = 0;
        return 0;
    }
}

static cmd_result_t run_cmd(char *const argv[], int timeout_sec, const char *env_lib_dir,
                            const char *work_dir) {
    cmd_result_t r;
    char out_tpl[] = "/tmp/liric_cmd_out_XXXXXX";
    char err_tpl[] = "/tmp/liric_cmd_err_XXXXXX";
    int out_fd, err_fd;
    int status = 0;
    pid_t pid;
    double t0;

    r.rc = -1;
    r.stdout_text = xstrdup("");
    r.stderr_text = xstrdup("");
    r.elapsed_ms = 0.0;
    r.timed_out = 0;

    out_fd = mkstemp(out_tpl);
    if (out_fd < 0) die("mkstemp failed", NULL);
    err_fd = mkstemp(err_tpl);
    if (err_fd < 0) die("mkstemp failed", NULL);

    pid = fork();
    if (pid < 0) die("fork failed", NULL);

    if (pid == 0) {
        if (work_dir && chdir(work_dir) != 0) _exit(127);
        if (env_lib_dir) {
            setenv("DYLD_LIBRARY_PATH", env_lib_dir, 1);
            setenv("LD_LIBRARY_PATH", env_lib_dir, 1);
        }
        {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        if (dup2(out_fd, STDOUT_FILENO) < 0) _exit(127);
        if (dup2(err_fd, STDERR_FILENO) < 0) _exit(127);
        close(out_fd);
        close(err_fd);
        execvp(argv[0], argv);
        _exit(127);
    }

    t0 = now_ms();
    close(out_fd);
    close(err_fd);

    if (wait_with_timeout(pid, timeout_sec, &status) == 1) {
        r.timed_out = 1;
        r.rc = -99;
    } else if (WIFEXITED(status)) {
        r.rc = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.rc = -WTERMSIG(status);
    } else {
        r.rc = -1;
    }
    r.elapsed_ms = now_ms() - t0;

    free(r.stdout_text);
    free(r.stderr_text);
    r.stdout_text = read_all_file(out_tpl);
    r.stderr_text = read_all_file(err_tpl);
    unlink(out_tpl);
    unlink(err_tpl);

    return r;
}

static void free_cmd_result(cmd_result_t *r) {
    free(r->stdout_text);
    free(r->stderr_text);
    r->stdout_text = NULL;
    r->stderr_text = NULL;
}

static void strlist_init(strlist_t *l) {
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

static void strlist_push(strlist_t *l, const char *s) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 64;
        char **nitems = (char **)realloc(l->items, ncap * sizeof(char *));
        if (!nitems) die("out of memory", NULL);
        l->items = nitems;
        l->cap = ncap;
    }
    l->items[l->n++] = xstrdup(s);
}

static void strlist_free(strlist_t *l) {
    size_t i;
    for (i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static int cmp_double(const void *a, const void *b) {
    const double *da = (const double *)a;
    const double *db = (const double *)b;
    if (*da < *db) return -1;
    if (*da > *db) return 1;
    return 0;
}

static double median(const double *vals, size_t n) {
    double *tmp;
    double out;
    if (n == 0) return 0.0;
    tmp = (double *)malloc(n * sizeof(double));
    if (!tmp) die("out of memory", NULL);
    memcpy(tmp, vals, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double);
    if (n % 2 == 0) out = 0.5 * (tmp[n / 2 - 1] + tmp[n / 2]);
    else out = tmp[n / 2];
    free(tmp);
    return out;
}

static double percentile(const double *vals, size_t n, double p) {
    double *tmp;
    double k, frac, out;
    size_t f, c;
    if (n == 0) return 0.0;
    tmp = (double *)malloc(n * sizeof(double));
    if (!tmp) die("out of memory", NULL);
    memcpy(tmp, vals, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double);
    k = ((double)(n - 1)) * p / 100.0;
    f = (size_t)k;
    c = (f + 1 < n) ? (f + 1) : f;
    frac = k - (double)f;
    out = tmp[f] + frac * (tmp[c] - tmp[f]);
    free(tmp);
    return out;
}

static void rowlist_push(rowlist_t *l, row_t row) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 64;
        row_t *nitems = (row_t *)realloc(l->items, ncap * sizeof(row_t));
        if (!nitems) die("out of memory", NULL);
        l->items = nitems;
        l->cap = ncap;
    }
    l->items[l->n++] = row;
}

static void rowlist_free(rowlist_t *l) {
    size_t i;
    for (i = 0; i < l->n; i++) free(l->items[i].name);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static void optlist_push(optlist_t *l, name_opt_t entry) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 64;
        name_opt_t *nitems = (name_opt_t *)realloc(l->items, ncap * sizeof(name_opt_t));
        if (!nitems) die("out of memory", NULL);
        l->items = nitems;
        l->cap = ncap;
    }
    l->items[l->n++] = entry;
}

static void optlist_free(optlist_t *l) {
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->items[i].name);
        free(l->items[i].options);
    }
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static const char *optlist_find(const optlist_t *l, const char *name) {
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->items[i].name, name) == 0)
            return l->items[i].options;
    }
    return NULL;
}

static optlist_t parse_options_jsonl(const char *path) {
    optlist_t out;
    FILE *f;
    char line[4096];

    out.items = NULL;
    out.n = out.cap = 0;

    f = fopen(path, "r");
    if (!f) return out;

    while (fgets(line, sizeof(line), f)) {
        const char *np, *op;
        char *name_start, *name_end, *opts_start, *opts_end;
        name_opt_t entry;

        np = strstr(line, "\"name\":\"");
        if (!np) continue;
        name_start = (char *)np + 8;
        name_end = strchr(name_start, '"');
        if (!name_end) continue;

        op = strstr(line, "\"options\":\"");
        if (!op) continue;
        opts_start = (char *)op + 11;
        opts_end = strchr(opts_start, '"');
        if (!opts_end) continue;

        *name_end = '\0';
        *opts_end = '\0';
        entry.name = xstrdup(name_start);
        entry.options = xstrdup(opts_start);
        optlist_push(&out, entry);
    }

    fclose(f);
    return out;
}

static strlist_t tokenize_options(const char *opts) {
    strlist_t toks;
    size_t i = 0, n;

    strlist_init(&toks);
    if (!opts) return toks;
    n = strlen(opts);

    while (i < n) {
        char *tok;
        size_t t = 0;
        while (i < n && isspace((unsigned char)opts[i])) i++;
        if (i >= n) break;

        tok = (char *)malloc(n - i + 1);
        if (!tok) die("out of memory", NULL);

        if (opts[i] == '\'') {
            i++;
            while (i < n) {
                if (opts[i] == '\'' && i + 3 < n && opts[i+1] == '\\' && opts[i+2] == '\'' && opts[i+3] == '\'') {
                    tok[t++] = '\'';
                    i += 4;
                } else if (opts[i] == '\'') {
                    i++;
                    break;
                } else {
                    tok[t++] = opts[i++];
                }
            }
        } else {
            while (i < n && !isspace((unsigned char)opts[i]))
                tok[t++] = opts[i++];
        }
        tok[t] = '\0';
        strlist_push(&toks, tok);
        free(tok);
    }

    return toks;
}

static char *json_escape(const char *s) {
    size_t i, n = 0;
    char *out, *p;
    for (i = 0; s && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' || c == '"' || c == '\n' || c == '\r' || c == '\t') n += 2;
        else if (c < 0x20) n += 6;
        else n += 1;
    }
    out = (char *)malloc(n + 1);
    if (!out) die("out of memory", NULL);
    p = out;
    for (i = 0; s && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') { *p++ = '\\'; *p++ = '\\'; }
        else if (c == '"') { *p++ = '\\'; *p++ = '"'; }
        else if (c == '\n') { *p++ = '\\'; *p++ = 'n'; }
        else if (c == '\r') { *p++ = '\\'; *p++ = 'r'; }
        else if (c == '\t') { *p++ = '\\'; *p++ = 't'; }
        else if (c < 0x20) {
            sprintf(p, "\\u%04x", c);
            p += 6;
        } else *p++ = (char)c;
    }
    *p = '\0';
    return out;
}

static void resolve_default_compat_artifacts(const char *bench_dir, char **compat_path, char **opts_path) {
    char *compat_100 = path_join2(bench_dir, "compat_api_100.txt");
    char *opts_100 = path_join2(bench_dir, "compat_api_100_options.jsonl");
    char *compat_all = path_join2(bench_dir, "compat_api.txt");
    char *opts_all = path_join2(bench_dir, "compat_api_options.jsonl");

    if (file_exists(compat_100) && file_exists(opts_100)) {
        *compat_path = compat_100;
        *opts_path = opts_100;
        free(compat_all);
        free(opts_all);
    } else {
        *compat_path = compat_all;
        *opts_path = opts_all;
        free(compat_100);
        free(opts_100);
    }
}

static void write_json_success_row(FILE *f,
                                   const char *name,
                                   size_t iters_done,
                                   double lw,
                                   double ew,
                                   double lc,
                                   double ec,
                                   double lr,
                                   double er,
                                   double wall_sp,
                                   double compile_sp,
                                   double run_sp) {
    char *en = json_escape(name ? name : "");
    fprintf(f,
            "{\"name\":\"%s\",\"status\":\"ok\",\"iters\":%zu,"
            "\"liric_wall_median_ms\":%.6f,\"llvm_wall_median_ms\":%.6f,"
            "\"liric_compile_median_ms\":%.6f,\"llvm_compile_median_ms\":%.6f,"
            "\"liric_run_median_ms\":%.6f,\"llvm_run_median_ms\":%.6f,"
            "\"wall_speedup\":%.6f,\"compile_speedup\":%.6f,\"run_speedup\":%.6f}\n",
            en, iters_done, lw, ew, lc, ec, lr, er, wall_sp, compile_sp, run_sp);
    free(en);
}

static void write_json_skip_row(FILE *f, const char *name, const char *reason) {
    char *en = json_escape(name ? name : "");
    char *er = json_escape(reason ? reason : "unknown");
    fprintf(f,
            "{\"name\":\"%s\",\"status\":\"skipped\",\"reason\":\"%s\"}\n",
            en, er);
    free(en);
    free(er);
}

#define SKIP_REASON_COUNT 10
static const char *k_skip_reasons[SKIP_REASON_COUNT] = {
    "workdir_create_failed",
    "source_missing",
    "llvm_compile_failed",
    "llvm_compile_timeout",
    "llvm_run_failed",
    "llvm_run_timeout",
    "liric_compile_failed",
    "liric_compile_timeout",
    "liric_run_failed",
    "liric_run_timeout"
};

static int skip_reason_index(const char *reason) {
    size_t i;
    if (!reason) return -1;
    for (i = 0; i < SKIP_REASON_COUNT; i++) {
        if (strcmp(k_skip_reasons[i], reason) == 0) return (int)i;
    }
    return -1;
}

static void usage(void) {
    printf("usage: bench_api [options]\n");
    printf("  --lfortran PATH       path to lfortran+LLVM binary (default: ../lfortran/build/src/bin/lfortran)\n");
    printf("  --lfortran-liric PATH path to lfortran+liric binary (default: ../lfortran/build-liric/src/bin/lfortran)\n");
    printf("  --probe-runner PATH   path to liric_probe_runner (default: build/liric_probe_runner)\n");
    printf("  --runtime-lib PATH    path to liblfortran_runtime (auto-detected)\n");
    printf("  --test-dir PATH       path to integration_tests/ dir\n");
    printf("  --bench-dir PATH      output directory (default: /tmp/liric_bench)\n");
    printf("  --compat-list PATH    compat list file (default: compat_api_100.txt if present, else compat_api.txt)\n");
    printf("  --options-jsonl PATH  options jsonl file (default matches chosen compat list)\n");
    printf("  --iters N             iterations per test (default: 3)\n");
    printf("  --timeout N           per-command timeout in seconds (default: 30)\n");
    printf("  --min-completed N     fail if completed tests < N (default: 0)\n");
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    const char *default_runtime_dylib = "../lfortran/build/src/runtime/liblfortran_runtime.dylib";
    const char *default_runtime_so = "../lfortran/build/src/runtime/liblfortran_runtime.so";

    cfg.lfortran = "../lfortran/build/src/bin/lfortran";
    cfg.lfortran_liric = "../lfortran/build-liric/src/bin/lfortran";
    cfg.probe_runner = "build/liric_probe_runner";
    cfg.runtime_lib = file_exists(default_runtime_dylib) ? default_runtime_dylib : default_runtime_so;
    cfg.test_dir = "../lfortran/integration_tests";
    cfg.bench_dir = "/tmp/liric_bench";
    cfg.compat_list = NULL;
    cfg.options_jsonl = NULL;
    cfg.iters = 3;
    cfg.timeout_sec = 30;
    cfg.min_completed = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else if (strcmp(argv[i], "--lfortran") == 0 && i + 1 < argc) {
            cfg.lfortran = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-liric") == 0 && i + 1 < argc) {
            cfg.lfortran_liric = argv[++i];
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg.probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            cfg.runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--test-dir") == 0 && i + 1 < argc) {
            cfg.test_dir = argv[++i];
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            cfg.bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--compat-list") == 0 && i + 1 < argc) {
            cfg.compat_list = argv[++i];
        } else if (strcmp(argv[i], "--options-jsonl") == 0 && i + 1 < argc) {
            cfg.options_jsonl = argv[++i];
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            cfg.iters = atoi(argv[++i]);
            if (cfg.iters <= 0) cfg.iters = 3;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0) cfg.timeout_sec = 30;
        } else if (strcmp(argv[i], "--min-completed") == 0 && i + 1 < argc) {
            cfg.min_completed = atoi(argv[++i]);
            if (cfg.min_completed < 0) cfg.min_completed = 0;
        } else {
            die("unknown argument", argv[i]);
        }
    }

    if (!file_exists(cfg.lfortran)) die("lfortran (LLVM) not found", cfg.lfortran);
    if (!file_exists(cfg.lfortran_liric)) die("lfortran (liric) not found", cfg.lfortran_liric);
    if (!file_exists(cfg.probe_runner)) die("probe runner not found", cfg.probe_runner);
    if (!file_exists(cfg.runtime_lib)) die("runtime lib not found", cfg.runtime_lib);

    cfg.lfortran = to_abs_path(cfg.lfortran);
    cfg.lfortran_liric = to_abs_path(cfg.lfortran_liric);
    cfg.probe_runner = to_abs_path(cfg.probe_runner);
    cfg.runtime_lib = to_abs_path(cfg.runtime_lib);
    cfg.test_dir = to_abs_path(cfg.test_dir);
    cfg.bench_dir = to_abs_path(cfg.bench_dir);
    if (cfg.compat_list) cfg.compat_list = to_abs_path(cfg.compat_list);
    if (cfg.options_jsonl) cfg.options_jsonl = to_abs_path(cfg.options_jsonl);

    return cfg;
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    char *compat_path = NULL;
    char *opts_path = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (cfg.compat_list) compat_path = xstrdup(cfg.compat_list);
    if (cfg.options_jsonl) opts_path = xstrdup(cfg.options_jsonl);
    if (!compat_path || !opts_path) {
        char *default_compat = NULL;
        char *default_opts = NULL;
        resolve_default_compat_artifacts(cfg.bench_dir, &default_compat, &default_opts);
        if (!compat_path) compat_path = default_compat;
        else free(default_compat);
        if (!opts_path) opts_path = default_opts;
        else free(default_opts);
    }

    char *api_bin_dir = path_join2(cfg.bench_dir, "api_bin");
    char *jsonl_path = path_join2(cfg.bench_dir, "bench_api.jsonl");
    char *summary_path = path_join2(cfg.bench_dir, "bench_api_summary.json");
    FILE *f;
    strlist_t tests;
    optlist_t opts;
    rowlist_t rows;
    size_t skip_reason_counts[SKIP_REASON_COUNT];
    size_t i;
    int exit_code = 0;

    strlist_init(&tests);
    rows.items = NULL;
    rows.n = rows.cap = 0;
    memset(skip_reason_counts, 0, sizeof(skip_reason_counts));

    if (!file_exists(compat_path))
        die("compat list missing (run bench_compat_check first)", compat_path);
    if (!file_exists(opts_path))
        die("compat options missing (run bench_compat_check first)", opts_path);

    f = fopen(compat_path, "r");
    if (!f) die("failed to open compat list", compat_path);
    {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (n > 0) strlist_push(&tests, line);
        }
    }
    fclose(f);

    opts = parse_options_jsonl(opts_path);

    ensure_dir(api_bin_dir);

    printf("Benchmarking %zu tests, %d iterations each\n", tests.n, cfg.iters);
    printf("  lfortran LLVM:  %s\n", cfg.lfortran);
    printf("  lfortran liric: %s\n", cfg.lfortran_liric);
    printf("  probe_runner:   %s\n", cfg.probe_runner);
    printf("  runtime_lib:    %s\n", cfg.runtime_lib);
    printf("  test_dir:       %s\n", cfg.test_dir);
    printf("  bench_dir:      %s\n", cfg.bench_dir);
    printf("  compat_list:    %s\n", compat_path);
    printf("  options_jsonl:  %s\n", opts_path);
    printf("  min_completed:  %d\n", cfg.min_completed);

    {
        FILE *jf = fopen(jsonl_path, "w");
        if (!jf) die("failed to open output", jsonl_path);

        for (i = 0; i < tests.n; i++) {
            const char *name = tests.items[i];
            const char *test_opts = optlist_find(&opts, name);
            strlist_t opt_toks;
            char *source_path = NULL;
            char *bin_path = NULL;
            size_t it;
            double *liric_wall = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_wall = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_compile = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_run = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_compile = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_run = (double *)calloc((size_t)cfg.iters, sizeof(double));
            size_t ok_n = 0;
            const char *skip_reason = NULL;
            char work_tpl[PATH_MAX];
            const char *work_dir = NULL;

            strlist_init(&opt_toks);
            {
                int n = snprintf(work_tpl, sizeof(work_tpl), "%s/%s", cfg.bench_dir, "work_api_XXXXXX");
                if (n < 0 || (size_t)n >= sizeof(work_tpl)) {
                    skip_reason = "workdir_template_too_long";
                    goto skip_test;
                }
            }
            if (!mkdtemp(work_tpl)) {
                skip_reason = "workdir_create_failed";
                goto skip_test;
            }
            work_dir = work_tpl;

            opt_toks = tokenize_options(test_opts);

            {
                char fname[512];
                snprintf(fname, sizeof(fname), "%s.f90", name);
                source_path = path_join2(cfg.test_dir, fname);
            }

            bin_path = path_join2(api_bin_dir, name);

            if (!file_exists(source_path)) {
                skip_reason = "source_missing";
                goto skip_test;
            }

            for (it = 0; it < (size_t)cfg.iters; it++) {
                char **compile_argv;
                char *run_argv[2];
                char **liric_compile_argv;
                char *liric_run_argv[2];
                char *liric_bin_path;
                cmd_result_t compile_r, run_r, liric_compile_r, liric_run_r;
                double compile_ms, run_ms, liric_compile_ms, liric_run_ms;
                size_t argc_compile, j;

                /* --- LLVM side: lfortran+LLVM compile + run --- */
                argc_compile = 4 + opt_toks.n + 1;
                compile_argv = (char **)calloc(argc_compile + 1, sizeof(char *));
                if (!compile_argv) die("out of memory", NULL);
                compile_argv[0] = (char *)cfg.lfortran;
                compile_argv[1] = "--no-color";
                for (j = 0; j < opt_toks.n; j++)
                    compile_argv[2 + j] = opt_toks.items[j];
                compile_argv[2 + opt_toks.n] = source_path;
                compile_argv[3 + opt_toks.n] = "-o";
                compile_argv[4 + opt_toks.n] = bin_path;
                compile_argv[5 + opt_toks.n] = NULL;

                compile_r = run_cmd(compile_argv, cfg.timeout_sec, NULL, work_dir);
                free(compile_argv);
                if (compile_r.rc != 0) {
                    skip_reason = compile_r.timed_out ? "llvm_compile_timeout" : "llvm_compile_failed";
                    free_cmd_result(&compile_r);
                    break;
                }
                compile_ms = compile_r.elapsed_ms;
                free_cmd_result(&compile_r);

                run_argv[0] = bin_path;
                run_argv[1] = NULL;
                run_r = run_cmd((char *const *)run_argv, cfg.timeout_sec, NULL, work_dir);
                if (run_r.rc != 0) {
                    skip_reason = run_r.timed_out ? "llvm_run_timeout" : "llvm_run_failed";
                    free_cmd_result(&run_r);
                    break;
                }
                run_ms = run_r.elapsed_ms;
                free_cmd_result(&run_r);

                llvm_wall[ok_n] = compile_ms + run_ms;
                llvm_compile[ok_n] = compile_ms;
                llvm_run[ok_n] = run_ms;

                /* --- Liric side: lfortran+liric compile + run --- */
                {
                    char liric_bin_name[512];
                    snprintf(liric_bin_name, sizeof(liric_bin_name), "%s_liric", name);
                    liric_bin_path = path_join2(api_bin_dir, liric_bin_name);
                }

                liric_compile_argv = (char **)calloc(argc_compile + 1, sizeof(char *));
                if (!liric_compile_argv) die("out of memory", NULL);
                liric_compile_argv[0] = (char *)cfg.lfortran_liric;
                liric_compile_argv[1] = "--no-color";
                for (j = 0; j < opt_toks.n; j++)
                    liric_compile_argv[2 + j] = opt_toks.items[j];
                liric_compile_argv[2 + opt_toks.n] = source_path;
                liric_compile_argv[3 + opt_toks.n] = "-o";
                liric_compile_argv[4 + opt_toks.n] = liric_bin_path;
                liric_compile_argv[5 + opt_toks.n] = NULL;

                liric_compile_r = run_cmd(liric_compile_argv, cfg.timeout_sec, NULL, work_dir);
                free(liric_compile_argv);
                if (liric_compile_r.rc != 0) {
                    skip_reason = liric_compile_r.timed_out ? "liric_compile_timeout" : "liric_compile_failed";
                    free_cmd_result(&liric_compile_r);
                    free(liric_bin_path);
                    break;
                }
                liric_compile_ms = liric_compile_r.elapsed_ms;
                free_cmd_result(&liric_compile_r);

                liric_run_argv[0] = liric_bin_path;
                liric_run_argv[1] = NULL;
                liric_run_r = run_cmd((char *const *)liric_run_argv, cfg.timeout_sec, NULL, work_dir);
                if (liric_run_r.rc != 0) {
                    skip_reason = liric_run_r.timed_out ? "liric_run_timeout" : "liric_run_failed";
                    free_cmd_result(&liric_run_r);
                    free(liric_bin_path);
                    break;
                }
                liric_run_ms = liric_run_r.elapsed_ms;
                free_cmd_result(&liric_run_r);

                liric_wall[ok_n] = liric_compile_ms + liric_run_ms;
                liric_compile[ok_n] = liric_compile_ms;
                liric_run[ok_n] = liric_run_ms;
                free(liric_bin_path);

                ok_n++;
            }

            if (ok_n == 0) {
                if (!skip_reason) skip_reason = "llvm_compile_failed";
                goto skip_test;
            }

            {
                double lw = median(liric_wall, ok_n);
                double ew = median(llvm_wall, ok_n);
                double lc = median(liric_compile, ok_n);
                double ec = median(llvm_compile, ok_n);
                double lr = median(liric_run, ok_n);
                double er = median(llvm_run, ok_n);
                double wall_sp = lw > 0 ? ew / lw : 0.0;
                double compile_sp = lc > 0 ? ec / lc : 0.0;
                double run_sp = lr > 0 ? er / lr : 0.0;
                row_t row;

                row.name = xstrdup(name);
                row.liric_wall_ms = lw;
                row.llvm_wall_ms = ew;
                row.liric_compile_ms = lc;
                row.liric_run_ms = lr;
                row.llvm_compile_ms = ec;
                row.llvm_run_ms = er;
                rowlist_push(&rows, row);

                write_json_success_row(jf, name, ok_n, lw, ew, lc, ec, lr, er, wall_sp, compile_sp, run_sp);
                printf("  [%zu/%zu] %s: wall %.1fms vs %.1fms (%.2fx), compile %.1fms vs %.1fms (%.2fx)\n",
                       i + 1, tests.n, name, lw, ew, wall_sp, lc, ec, compile_sp);
            }
            goto next_test;

skip_test:
            {
                int idx = skip_reason_index(skip_reason);
                if (idx >= 0) skip_reason_counts[(size_t)idx]++;
            }
            write_json_skip_row(jf, name, skip_reason ? skip_reason : "unknown");
            printf("  [%zu/%zu] %s: skipped (%s)\n", i + 1, tests.n, name, skip_reason ? skip_reason : "unknown");

next_test:
            if (work_dir) rmdir(work_dir);
            free(source_path);
            free(bin_path);
            free(liric_wall);
            free(llvm_wall);
            free(liric_compile);
            free(liric_run);
            free(llvm_compile);
            free(llvm_run);
            strlist_free(&opt_toks);
        }

        fclose(jf);
    }

    if (rows.n > 0) {
        double *lw = (double *)malloc(rows.n * sizeof(double));
        double *ew = (double *)malloc(rows.n * sizeof(double));
        double *lc = (double *)malloc(rows.n * sizeof(double));
        double *ec = (double *)malloc(rows.n * sizeof(double));
        double *lr = (double *)malloc(rows.n * sizeof(double));
        double *er = (double *)malloc(rows.n * sizeof(double));
        double *wall_sp = (double *)malloc(rows.n * sizeof(double));
        double *compile_sp = (double *)malloc(rows.n * sizeof(double));
        double *run_sp = (double *)malloc(rows.n * sizeof(double));
        size_t j;
        size_t wall_faster = 0;
        size_t compile_faster = 0;
        size_t run_faster = 0;
        double sum_lw = 0.0, sum_ew = 0.0;
        double sum_lc = 0.0, sum_ec = 0.0;
        double sum_lr = 0.0, sum_er = 0.0;

        for (j = 0; j < rows.n; j++) {
            lw[j] = rows.items[j].liric_wall_ms;
            ew[j] = rows.items[j].llvm_wall_ms;
            lc[j] = rows.items[j].liric_compile_ms;
            ec[j] = rows.items[j].llvm_compile_ms;
            lr[j] = rows.items[j].liric_run_ms;
            er[j] = rows.items[j].llvm_run_ms;
            wall_sp[j] = lw[j] > 0 ? ew[j] / lw[j] : 0.0;
            compile_sp[j] = lc[j] > 0 ? ec[j] / lc[j] : 0.0;
            run_sp[j] = lr[j] > 0 ? er[j] / lr[j] : 0.0;
            if (wall_sp[j] > 1.0) wall_faster++;
            if (compile_sp[j] > 1.0) compile_faster++;
            if (run_sp[j] > 1.0) run_faster++;
            sum_lw += lw[j];
            sum_ew += ew[j];
            sum_lc += lc[j];
            sum_ec += ec[j];
            sum_lr += lr[j];
            sum_er += er[j];
        }

        printf("\n========================================================================\n");
        printf("  lfortran+LLVM vs lfortran+liric (API path)\n");
        printf("  %zu tests, %d iterations each\n", rows.n, cfg.iters);
        printf("========================================================================\n");

        printf("\n  WALL-CLOCK (compile+link+run vs compile+JIT+run)\n");
        printf("  Median:    liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(lw, rows.n), median(ew, rows.n), median(wall_sp, rows.n));
        printf("  Mean-ish:  aggregate %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lw, sum_ew, (sum_lw > 0 ? sum_ew / sum_lw : 0.0));
        printf("  P90/P95:   %.2fx / %.2fx\n", percentile(wall_sp, rows.n, 90), percentile(wall_sp, rows.n, 95));
        printf("  Faster:    %zu/%zu (%.1f%%)\n", wall_faster, rows.n, 100.0 * (double)wall_faster / (double)rows.n);

        printf("\n  PHASE BREAKDOWN (subprocess medians)\n");
        printf("  Compile:   liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(lc, rows.n), median(ec, rows.n), median(compile_sp, rows.n));
        printf("  Compile+:  aggregate %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lc, sum_ec, (sum_lc > 0 ? sum_ec / sum_lc : 0.0));
        printf("  Compile F: %zu/%zu (%.1f%%)\n",
               compile_faster, rows.n, 100.0 * (double)compile_faster / (double)rows.n);
        printf("  Run:       liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(lr, rows.n), median(er, rows.n), median(run_sp, rows.n));
        printf("  Run+:      aggregate %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lr, sum_er, (sum_lr > 0 ? sum_er / sum_lr : 0.0));
        printf("  Run F:     %zu/%zu (%.1f%%)\n",
               run_faster, rows.n, 100.0 * (double)run_faster / (double)rows.n);

        printf("\n  Results: %s\n", jsonl_path);

        free(lw);
        free(ew);
        free(lc);
        free(ec);
        free(lr);
        free(er);
        free(wall_sp);
        free(compile_sp);
        free(run_sp);
    }
    {
        size_t attempted = tests.n;
        size_t completed = rows.n;
        size_t skipped = (attempted >= completed) ? (attempted - completed) : 0;
        FILE *sf = fopen(summary_path, "w");
        if (!sf) die("failed to open output", summary_path);

        fprintf(sf, "{\n");
        fprintf(sf, "  \"attempted\": %zu,\n", attempted);
        fprintf(sf, "  \"completed\": %zu,\n", completed);
        fprintf(sf, "  \"skipped\": %zu,\n", skipped);
        fprintf(sf, "  \"iters\": %d,\n", cfg.iters);
        fprintf(sf, "  \"min_completed\": %d,\n", cfg.min_completed);
        fprintf(sf, "  \"completion_threshold_met\": %s,\n",
                completed >= (size_t)cfg.min_completed ? "true" : "false");
        {
            char *ec = json_escape(compat_path);
            char *eo = json_escape(opts_path);
            fprintf(sf, "  \"compat_list\": \"%s\",\n", ec);
            fprintf(sf, "  \"options_jsonl\": \"%s\",\n", eo);
            free(ec);
            free(eo);
        }
        fprintf(sf, "  \"skip_reasons\": {\n");
        for (i = 0; i < SKIP_REASON_COUNT; i++) {
            fprintf(sf, "    \"%s\": %zu%s\n",
                    k_skip_reasons[i],
                    skip_reason_counts[i],
                    (i + 1 == SKIP_REASON_COUNT) ? "" : ",");
        }
        fprintf(sf, "  }\n");
        fprintf(sf, "}\n");
        fclose(sf);

        printf("\n  Accounting: attempted=%zu completed=%zu skipped=%zu\n",
               attempted, completed, skipped);
        for (i = 0; i < SKIP_REASON_COUNT; i++) {
            if (skip_reason_counts[i] > 0) {
                printf("    skip[%s]=%zu\n", k_skip_reasons[i], skip_reason_counts[i]);
            }
        }
        printf("  Summary: %s\n", summary_path);

        if (completed == 0) {
            fprintf(stderr, "no benchmark results completed\n");
            exit_code = 1;
        }
        if (completed < (size_t)cfg.min_completed) {
            fprintf(stderr, "completion gate failed: completed=%zu < min_completed=%d\n",
                    completed, cfg.min_completed);
            exit_code = 1;
        }
    }

    free(compat_path);
    free(opts_path);
    free(api_bin_dir);
    free(jsonl_path);
    free(summary_path);
    strlist_free(&tests);
    optlist_free(&opts);
    rowlist_free(&rows);
    return exit_code;
}
