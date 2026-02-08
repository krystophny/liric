// LL benchmark: liric_probe_runner vs lli wall-clock + fair JIT-internal phases.
// C replacement for tools/bench_ll.py with fair in-process LLVM timing.

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
    const char *probe_runner;
    const char *runtime_lib;
    const char *lli;
    const char *lli_phases;
    const char *bench_dir;
    int iters;
    int timeout_sec;
} cfg_t;

typedef struct {
    char *name;
    double liric_wall_ms;
    double lli_wall_ms;
    double liric_parse_ms;
    double liric_compile_ms;
    double lli_parse_ms;
    double lli_jit_ms;
    double lli_lookup_ms;
    double liric_internal_ms;
    double lli_internal_ms;
    double lli_internal_with_lookup_ms;
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

static void die(const char *msg, const char *path) {
    if (path) fprintf(stderr, "%s: %s\n", msg, path);
    else fprintf(stderr, "%s\n", msg);
    exit(1);
}

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
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

static char *dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t n;
    char *out;
    if (!slash) return xstrdup(".");
    n = (size_t)(slash - path);
    if (n == 0) n = 1;
    out = (char *)malloc(n + 1);
    if (!out) die("out of memory", NULL);
    memcpy(out, path, n);
    out[n] = '\0';
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
    double start = now_ms();
    int status;
    while (1) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            *status_out = status;
            return 0;
        }
        if (r < 0) {
            *status_out = 0;
            return -1;
        }
        if ((now_ms() - start) > timeout_sec * 1000.0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            *status_out = status;
            return 1;
        }
        usleep(10000);
    }
}

static cmd_result_t run_cmd(char *const argv[], int timeout_sec, const char *env_lib_dir) {
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
        if (env_lib_dir) {
            setenv("DYLD_LIBRARY_PATH", env_lib_dir, 1);
            setenv("LD_LIBRARY_PATH", env_lib_dir, 1);
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

static int parse_probe_timing(const char *stderr_text, double *out_parse_ms, double *out_compile_ms) {
    const char *p = strstr(stderr_text, "TIMING ");
    double read_us, parse_us, jit_create_us, load_lib_us, compile_us, total_us;
    if (!p) return 0;
    if (sscanf(p,
               "TIMING read_us=%lf parse_us=%lf jit_create_us=%lf load_lib_us=%lf compile_us=%lf total_us=%lf",
               &read_us, &parse_us, &jit_create_us, &load_lib_us, &compile_us, &total_us) == 6) {
        (void)read_us;
        (void)jit_create_us;
        (void)load_lib_us;
        (void)total_us;
        *out_parse_ms = parse_us / 1000.0;
        *out_compile_ms = compile_us / 1000.0;
        return 1;
    }
    return 0;
}

static int json_get_number(const char *json, const char *key, double *out_val) {
    const char *p = strstr(json, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (!*p) return 0;
    *out_val = strtod(p, NULL);
    return 1;
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

static void usage(void) {
    printf("usage: bench_ll [options]\n");
    printf("  --iters N             iterations per test (default: 3)\n");
    printf("  --timeout N           command timeout in seconds (default: 15)\n");
    printf("  --probe-runner PATH   path to liric_probe_runner\n");
    printf("  --runtime-lib PATH    path to liblfortran_runtime\n");
    printf("  --lli PATH            path to lli\n");
    printf("  --lli-phases PATH     path to bench_lli_phases\n");
    printf("  --bench-dir PATH      benchmark dir (default: /tmp/liric_bench)\n");
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    const char *default_runtime_dylib = "../lfortran/build/src/runtime/liblfortran_runtime.dylib";
    const char *default_runtime_so = "../lfortran/build/src/runtime/liblfortran_runtime.so";

    cfg.probe_runner = "build/liric_probe_runner";
    cfg.runtime_lib = file_exists(default_runtime_dylib) ? default_runtime_dylib : default_runtime_so;
    cfg.lli = "lli";
    cfg.lli_phases = "build/bench_lli_phases";
    cfg.bench_dir = "/tmp/liric_bench";
    cfg.iters = 3;
    cfg.timeout_sec = 15;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            cfg.iters = atoi(argv[++i]);
            if (cfg.iters <= 0) cfg.iters = 3;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0) cfg.timeout_sec = 15;
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg.probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            cfg.runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--lli") == 0 && i + 1 < argc) {
            cfg.lli = argv[++i];
        } else if (strcmp(argv[i], "--lli-phases") == 0 && i + 1 < argc) {
            cfg.lli_phases = argv[++i];
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            cfg.bench_dir = argv[++i];
        } else {
            die("unknown argument", argv[i]);
        }
    }

    if (!file_exists(cfg.probe_runner)) die("probe runner not found", cfg.probe_runner);
    if (!file_exists(cfg.runtime_lib)) die("runtime lib not found", cfg.runtime_lib);
    if (!file_exists(cfg.lli_phases)) die("bench_lli_phases not found", cfg.lli_phases);

    return cfg;
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    char *compat_path = path_join2(cfg.bench_dir, "compat_ll.txt");
    char *ll_dir = path_join2(cfg.bench_dir, "ll");
    char *jsonl_path = path_join2(cfg.bench_dir, "bench_ll.jsonl");
    char *runtime_dir = dirname_dup(cfg.runtime_lib);
    FILE *f;
    strlist_t tests;
    rowlist_t rows;
    size_t i;

    strlist_init(&tests);
    rows.items = NULL;
    rows.n = rows.cap = 0;

    if (!file_exists(compat_path)) {
        die("compat list missing (run bench_compat_check first)", compat_path);
    }

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

    printf("Benchmarking %zu tests, %d iterations each\n", tests.n, cfg.iters);

    {
        FILE *jf = fopen(jsonl_path, "w");
        if (!jf) die("failed to open output", jsonl_path);

        for (i = 0; i < tests.n; i++) {
            char *ll_base = path_join2(ll_dir, tests.items[i]);
            char *ll_path = (char *)malloc(strlen(ll_base) + 4);
            size_t it;
            double *liric_wall = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *lli_wall = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_parse = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_compile = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *lli_parse = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *lli_jit = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *lli_lookup = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_internal = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *lli_internal = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *lli_internal_with_lookup = (double *)calloc((size_t)cfg.iters, sizeof(double));
            size_t ok_n = 0;
            int skipped = 0;

            sprintf(ll_path, "%s.ll", ll_base);
            free(ll_base);

            if (!file_exists(ll_path)) {
                printf("  [%zu/%zu] %s: skipped (.ll missing)\n", i + 1, tests.n, tests.items[i]);
                free(ll_path);
                free(liric_wall);
                free(lli_wall);
                free(liric_parse);
                free(liric_compile);
                free(lli_parse);
                free(lli_jit);
                free(lli_lookup);
                free(liric_internal);
                free(lli_internal);
                free(lli_internal_with_lookup);
                continue;
            }

            for (it = 0; it < (size_t)cfg.iters; it++) {
                char *probe_argv[8];
                char *lli_argv[6];
                char *ph_argv[12];
                cmd_result_t rp, rl, ri;
                double parse_ms = 0.0, compile_ms = 0.0;
                double lli_parse_ms = 0.0, lli_jit_ms = 0.0, lli_lookup_ms = 0.0;

                probe_argv[0] = (char *)cfg.probe_runner;
                probe_argv[1] = "--timing";
                probe_argv[2] = "--sig";
                probe_argv[3] = "i32_argc_argv";
                probe_argv[4] = "--load-lib";
                probe_argv[5] = (char *)cfg.runtime_lib;
                probe_argv[6] = ll_path;
                probe_argv[7] = NULL;

                rp = run_cmd(probe_argv, cfg.timeout_sec, NULL);
                if (rp.rc < 0 || !parse_probe_timing(rp.stderr_text, &parse_ms, &compile_ms)) {
                    skipped = 1;
                    free_cmd_result(&rp);
                    break;
                }
                liric_wall[ok_n] = rp.elapsed_ms;
                liric_parse[ok_n] = parse_ms;
                liric_compile[ok_n] = compile_ms;
                liric_internal[ok_n] = parse_ms + compile_ms;
                free_cmd_result(&rp);

                lli_argv[0] = (char *)cfg.lli;
                lli_argv[1] = "-O0";
                lli_argv[2] = "--dlopen";
                lli_argv[3] = (char *)cfg.runtime_lib;
                lli_argv[4] = ll_path;
                lli_argv[5] = NULL;

                rl = run_cmd(lli_argv, cfg.timeout_sec, runtime_dir);
                if (rl.rc < 0) {
                    skipped = 1;
                    free_cmd_result(&rl);
                    break;
                }
                lli_wall[ok_n] = rl.elapsed_ms;
                free_cmd_result(&rl);

                ph_argv[0] = (char *)cfg.lli_phases;
                ph_argv[1] = "--json";
                ph_argv[2] = "--iters";
                ph_argv[3] = "1";
                ph_argv[4] = "--func";
                ph_argv[5] = "main";
                ph_argv[6] = "--sig";
                ph_argv[7] = "i32_argc_argv";
                ph_argv[8] = "--load-lib";
                ph_argv[9] = (char *)cfg.runtime_lib;
                ph_argv[10] = ll_path;
                ph_argv[11] = NULL;

                ri = run_cmd(ph_argv, cfg.timeout_sec, NULL);
                if (ri.rc != 0 ||
                    !json_get_number(ri.stdout_text, "\"parse_ms\"", &lli_parse_ms) ||
                    !json_get_number(ri.stdout_text, "\"jit_ms\"", &lli_jit_ms) ||
                    !json_get_number(ri.stdout_text, "\"lookup_ms\"", &lli_lookup_ms)) {
                    skipped = 1;
                    free_cmd_result(&ri);
                    break;
                }
                lli_parse[ok_n] = lli_parse_ms;
                lli_jit[ok_n] = lli_jit_ms;
                lli_lookup[ok_n] = lli_lookup_ms;
                lli_internal[ok_n] = lli_parse_ms + lli_jit_ms;
                lli_internal_with_lookup[ok_n] = lli_parse_ms + lli_jit_ms + lli_lookup_ms;
                free_cmd_result(&ri);

                ok_n++;
            }

            if (skipped || ok_n == 0) {
                printf("  [%zu/%zu] %s: skipped (runtime error)\n", i + 1, tests.n, tests.items[i]);
                free(ll_path);
                free(liric_wall);
                free(lli_wall);
                free(liric_parse);
                free(liric_compile);
                free(lli_parse);
                free(lli_jit);
                free(lli_lookup);
                free(liric_internal);
                free(lli_internal);
                free(lli_internal_with_lookup);
                continue;
            }

            {
                double lw = median(liric_wall, ok_n);
                double ew = median(lli_wall, ok_n);
                double lp = median(liric_parse, ok_n);
                double lc = median(liric_compile, ok_n);
                double ep = median(lli_parse, ok_n);
                double ej = median(lli_jit, ok_n);
                double el = median(lli_lookup, ok_n);
                double li = median(liric_internal, ok_n);
                double ei = median(lli_internal, ok_n);
                double eik = median(lli_internal_with_lookup, ok_n);
                double wall_sp = lw > 0 ? ew / lw : 0.0;
                double int_sp = li > 0 ? ei / li : 0.0;
                row_t row;

                row.name = xstrdup(tests.items[i]);
                row.liric_wall_ms = lw;
                row.lli_wall_ms = ew;
                row.liric_parse_ms = lp;
                row.liric_compile_ms = lc;
                row.lli_parse_ms = ep;
                row.lli_jit_ms = ej;
                row.lli_lookup_ms = el;
                row.liric_internal_ms = li;
                row.lli_internal_ms = ei;
                row.lli_internal_with_lookup_ms = eik;
                rowlist_push(&rows, row);

                fprintf(jf,
                    "{\"name\":\"%s\",\"iters\":%zu,"
                    "\"liric_wall_median_ms\":%.6f,\"lli_wall_median_ms\":%.6f,"
                    "\"wall_speedup\":%.6f,"
                    "\"liric_parse_median_ms\":%.6f,\"liric_compile_median_ms\":%.6f,"
                    "\"lli_parse_median_ms\":%.6f,\"lli_jit_median_ms\":%.6f,\"lli_lookup_median_ms\":%.6f,"
                    "\"liric_internal_median_ms\":%.6f,\"lli_internal_median_ms\":%.6f,"
                    "\"lli_internal_with_lookup_median_ms\":%.6f,"
                    "\"internal_speedup\":%.6f}\n",
                    tests.items[i], ok_n, lw, ew, wall_sp, lp, lc, ep, ej, el, li, ei, eik, int_sp);

                printf("  [%zu/%zu] %s: wall %.1fms vs %.1fms (%.2fx), internal %.3fms vs %.3fms (%.2fx)\n",
                       i + 1, tests.n, tests.items[i], lw, ew, wall_sp, li, ei, int_sp);
            }

            free(ll_path);
            free(liric_wall);
            free(lli_wall);
            free(liric_parse);
            free(liric_compile);
            free(lli_parse);
            free(lli_jit);
            free(lli_lookup);
            free(liric_internal);
            free(lli_internal);
            free(lli_internal_with_lookup);
        }

        fclose(jf);
    }

    if (rows.n == 0) {
        die("no benchmark results", NULL);
    }

    {
        double *lw = (double *)malloc(rows.n * sizeof(double));
        double *ew = (double *)malloc(rows.n * sizeof(double));
        double *lp = (double *)malloc(rows.n * sizeof(double));
        double *lc = (double *)malloc(rows.n * sizeof(double));
        double *ep = (double *)malloc(rows.n * sizeof(double));
        double *ej = (double *)malloc(rows.n * sizeof(double));
        double *el = (double *)malloc(rows.n * sizeof(double));
        double *li = (double *)malloc(rows.n * sizeof(double));
        double *ei = (double *)malloc(rows.n * sizeof(double));
        double *eik = (double *)malloc(rows.n * sizeof(double));
        double *wall_sp = (double *)malloc(rows.n * sizeof(double));
        double *int_sp = (double *)malloc(rows.n * sizeof(double));
        size_t j;
        size_t wall_faster = 0;
        size_t int_faster = 0;
        double sum_lw = 0.0, sum_ew = 0.0, sum_lp = 0.0, sum_lc = 0.0;
        double sum_ep = 0.0, sum_ej = 0.0, sum_el = 0.0;
        double sum_li = 0.0, sum_ei = 0.0, sum_eik = 0.0;
        char *summary_path = path_join2(cfg.bench_dir, "bench_ll_summary.json");

        for (j = 0; j < rows.n; j++) {
            lw[j] = rows.items[j].liric_wall_ms;
            ew[j] = rows.items[j].lli_wall_ms;
            lp[j] = rows.items[j].liric_parse_ms;
            lc[j] = rows.items[j].liric_compile_ms;
            ep[j] = rows.items[j].lli_parse_ms;
            ej[j] = rows.items[j].lli_jit_ms;
            el[j] = rows.items[j].lli_lookup_ms;
            li[j] = rows.items[j].liric_internal_ms;
            ei[j] = rows.items[j].lli_internal_ms;
            eik[j] = rows.items[j].lli_internal_with_lookup_ms;
            wall_sp[j] = lw[j] > 0 ? ew[j] / lw[j] : 0.0;
            int_sp[j] = li[j] > 0 ? ei[j] / li[j] : 0.0;
            if (wall_sp[j] > 1.0) wall_faster++;
            if (int_sp[j] > 1.0) int_faster++;
            sum_lw += lw[j];
            sum_ew += ew[j];
            sum_lp += lp[j];
            sum_lc += lc[j];
            sum_ep += ep[j];
            sum_ej += ej[j];
            sum_el += el[j];
            sum_li += li[j];
            sum_ei += ei[j];
            sum_eik += eik[j];
        }

        printf("\n========================================================================\n");
        printf("  liric JIT vs lli (LL-file path, -O0)\n");
        printf("  %zu tests, %d iterations each\n", rows.n, cfg.iters);
        printf("========================================================================\n");

        printf("\n  WALL-CLOCK (subprocess vs subprocess)\n");
        printf("  Median:    liric %.3f ms, lli %.3f ms, speedup %.2fx\n",
               median(lw, rows.n), median(ew, rows.n), median(wall_sp, rows.n));
        printf("  Mean-ish:  aggregate %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lw, sum_ew, (sum_lw > 0 ? sum_ew / sum_lw : 0.0));
        printf("  P90/P95:   %.2fx / %.2fx\n", percentile(wall_sp, rows.n, 90), percentile(wall_sp, rows.n, 95));
        printf("  Faster:    %zu/%zu (%.1f%%)\n", wall_faster, rows.n, 100.0 * (double)wall_faster / (double)rows.n);

        printf("\n  JIT-INTERNAL (fair in-process compare: parse+compile vs parse+compile)\n");
        printf("  Median:    liric %.6f ms, lli %.6f ms, speedup %.2fx\n",
               median(li, rows.n), median(ei, rows.n), median(int_sp, rows.n));
        printf("  Aggregate: %.3f ms vs %.3f ms, speedup %.2fx\n",
               sum_li, sum_ei, (sum_li > 0 ? sum_ei / sum_li : 0.0));
        printf("  Split:     liric parse %.3f ms + compile %.3f ms | lli parse %.3f ms + jit %.3f ms (+ lookup %.3f ms)\n",
               median(lp, rows.n), median(lc, rows.n), median(ep, rows.n), median(ej, rows.n), median(el, rows.n));
        printf("  Material.: lli parse+jit+lookup median %.6f ms (aggregate %.3f ms)\n",
               median(eik, rows.n), sum_eik);
        printf("  P90/P95:   %.2fx / %.2fx\n", percentile(int_sp, rows.n, 90), percentile(int_sp, rows.n, 95));
        printf("  Faster:    %zu/%zu (%.1f%%)\n", int_faster, rows.n, 100.0 * (double)int_faster / (double)rows.n);

        printf("\n  Results: %s\n", jsonl_path);
        {
            FILE *sf = fopen(summary_path, "w");
            if (sf) {
                fprintf(sf,
                        "{"
                        "\"tests\":%zu,"
                        "\"iters\":%d,"
                        "\"wall\":{"
                        "\"liric_median_ms\":%.6f,"
                        "\"lli_median_ms\":%.6f,"
                        "\"speedup_median\":%.6f,"
                        "\"liric_aggregate_ms\":%.6f,"
                        "\"lli_aggregate_ms\":%.6f"
                        "},"
                        "\"internal\":{"
                        "\"liric_median_ms\":%.6f,"
                        "\"lli_median_ms\":%.6f,"
                        "\"speedup_median\":%.6f,"
                        "\"liric_aggregate_ms\":%.6f,"
                        "\"lli_aggregate_ms\":%.6f,"
                        "\"liric_parse_median_ms\":%.6f,"
                        "\"liric_compile_median_ms\":%.6f,"
                        "\"lli_parse_median_ms\":%.6f,"
                        "\"lli_jit_median_ms\":%.6f,"
                        "\"lli_lookup_median_ms\":%.6f,"
                        "\"lli_parse_jit_lookup_median_ms\":%.6f,"
                        "\"lli_parse_jit_lookup_aggregate_ms\":%.6f"
                        "}"
                        "}\n",
                        rows.n, cfg.iters,
                        median(lw, rows.n), median(ew, rows.n), median(wall_sp, rows.n), sum_lw, sum_ew,
                        median(li, rows.n), median(ei, rows.n), median(int_sp, rows.n), sum_li, sum_ei,
                        median(lp, rows.n), median(lc, rows.n), median(ep, rows.n), median(ej, rows.n), median(el, rows.n),
                        median(eik, rows.n), sum_eik);
                fclose(sf);
            }
        }
        printf("  Summary: %s\n", summary_path);

        free(lw);
        free(ew);
        free(lp);
        free(lc);
        free(ep);
        free(ej);
        free(el);
        free(li);
        free(ei);
        free(eik);
        free(wall_sp);
        free(int_sp);
        free(summary_path);
    }

    free(compat_path);
    free(ll_dir);
    free(jsonl_path);
    free(runtime_dir);
    strlist_free(&tests);
    rowlist_free(&rows);
    return 0;
}
