// API benchmark (direct-JIT mode): compare lfortran --jit execution
// between LLVM build and WITH_LIRIC build (no object/link benchmark path).

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
    const char *lfortran;
    const char *lfortran_liric;
    const char *test_dir;
    const char *bench_dir;
    const char *compat_list;
    const char *options_jsonl;
    int iters;
    int timeout_ms;
    int min_completed;
    int keep_fail_workdirs;
    int fail_sample_limit;
    const char *fail_log_dir;
    double lookup_dispatch_share_pct;
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
    double frontend_ms;
    double liric_llvm_ir_ms;
    double llvm_llvm_ir_ms;
} row_t;

typedef struct {
    row_t *items;
    size_t n;
    size_t cap;
} rowlist_t;

typedef struct {
    double file_read_ms;
    double src_to_asr_ms;
    double asr_passes_ms;
    double asr_to_mod_ms;
    double llvm_ir_ms;
    double llvm_opt_ms;
    double llvm_to_jit_ms;
    double jit_run_ms;
    double total_ms;
} time_report_t;

typedef struct {
    const char *reason;
    const char *failing_side;
    int rc;
    int has_rc;
    int signal;
    int timed_out;
    size_t iteration;
    double elapsed_ms;
    int timeout_ms;
    size_t stdout_bytes;
    size_t stderr_bytes;
    size_t stdout_nonempty_lines;
    size_t stderr_nonempty_lines;
    int timeout_silent;
    size_t time_report_phase_count;
    double time_report_last_phase_ms;
    char *stdout_text;
    char *stderr_text;
    char *stdout_excerpt;
    char *stderr_excerpt;
    char *last_stdout_line;
    char *last_stderr_line;
    char *time_report_last_phase;
    char *work_dir;
    char *stdout_log_path;
    char *stderr_log_path;
} skip_diag_t;

#define TRACKER_TARGET_LLVM_IR_CREATION_MS 0.350
#define TRACKER_TARGET_LLVM_TO_JIT_MS 0.250
#define TRACKER_TARGET_RUN_SPEEDUP_AVG 15.0
#define TRACKER_TARGET_RUN_SPEEDUP_MIN 10.0
#define TRACKER_TARGET_LOOKUP_DISPATCH_PCT 0.25

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

static int remove_tree(const char *path) {
    DIR *dir;
    struct dirent *ent;
    char child[PATH_MAX];
    struct stat st;

    if (!path || path[0] == '\0') return 0;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    dir = opendir(path);
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        int n;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            closedir(dir);
            return -1;
        }
        if (remove_tree(child) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(path);
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

static int wait_with_timeout(pid_t pid, int timeout_ms, int *status_out) {
    int status;
    pid_t r;

    if (timeout_ms <= 0) {
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
        const double deadline_ms = now_ms() + (double)timeout_ms;
        for (;;) {
            r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                *status_out = status;
                return 0;
            }
            if (r == 0) {
                struct timespec ts;
                if (now_ms() >= deadline_ms) {
                    kill(pid, SIGKILL);
                    do {
                        r = waitpid(pid, &status, 0);
                    } while (r < 0 && errno == EINTR);
                    if (r < 0) {
                        *status_out = 0;
                        return -1;
                    }
                    *status_out = status;
                    return 1;
                }
                ts.tv_sec = 0;
                ts.tv_nsec = 1000000L; // 1ms polling interval
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EINTR) continue;
            *status_out = 0;
            return -1;
        }
    }
}

static cmd_result_t run_cmd(char *const argv[], int timeout_ms, const char *env_lib_dir,
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

    if (wait_with_timeout(pid, timeout_ms, &status) == 1) {
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

static cmd_result_t run_lfortran_jit_cmd(const char *lfortran_bin,
                                         const strlist_t *opt_toks,
                                         const char *extra_opt,
                                         const char *source_path,
                                         int timeout_ms,
                                         const char *work_dir) {
    size_t j;
    size_t extra_n = (extra_opt && extra_opt[0] != '\0') ? 1 : 0;
    size_t argc_jit = 5 + opt_toks->n + extra_n + 1;
    char **argv = (char **)calloc(argc_jit + 1, sizeof(char *));
    cmd_result_t r;
    if (!argv) die("out of memory", NULL);

    argv[0] = (char *)lfortran_bin;
    argv[1] = "--backend=llvm";
    argv[2] = "--jit";
    argv[3] = "--time-report";
    argv[4] = "--no-color";
    for (j = 0; j < opt_toks->n; j++)
        argv[5 + j] = opt_toks->items[j];
    if (extra_n) argv[5 + opt_toks->n] = (char *)extra_opt;
    argv[5 + opt_toks->n + extra_n] = (char *)source_path;
    argv[6 + opt_toks->n + extra_n] = NULL;

    r = run_cmd(argv, timeout_ms, NULL, work_dir);
    free(argv);
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

static void strlist_truncate(strlist_t *l, size_t keep_n) {
    size_t i;
    if (!l || keep_n >= l->n) return;
    for (i = keep_n; i < l->n; i++) free(l->items[i]);
    l->n = keep_n;
}

static void validate_compat_sources(const strlist_t *tests, const char *test_dir, const char *compat_path) {
    size_t i;
    size_t missing_count = 0;
    const size_t sample_limit = 20;
    for (i = 0; i < tests->n; i++) {
        char fname[512];
        int n = snprintf(fname, sizeof(fname), "%s.f90", tests->items[i]);
        char *source_path;
        if (n < 0 || (size_t)n >= sizeof(fname)) {
            if (missing_count < sample_limit) {
                fprintf(stderr, "compat entry too long (cannot resolve source): %s\n", tests->items[i]);
            }
            missing_count++;
            continue;
        }
        source_path = path_join2(test_dir, fname);
        if (!file_exists(source_path)) {
            if (missing_count < sample_limit)
                fprintf(stderr, "missing compat source: %s\n", source_path);
            missing_count++;
        }
        free(source_path);
    }
    if (missing_count > 0) {
        if (missing_count > sample_limit) {
            fprintf(stderr, "... and %zu more missing entries\n", missing_count - sample_limit);
        }
        fprintf(stderr,
                "compat list preflight failed: %zu stale entr%s under %s\n",
                missing_count, (missing_count == 1) ? "y" : "ies", test_dir);
        fprintf(stderr,
                "Remediation: regenerate compat artifacts, e.g. ./build/bench_compat_check --timeout 15\n");
        die("compat list contains stale entries; run bench_compat_check to refresh", compat_path);
    }
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

static int strlist_contains_exact(const strlist_t *l, const char *needle) {
    size_t i;
    if (!l || !needle) return 0;
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->items[i], needle) == 0) return 1;
    }
    return 0;
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

static char *normalize_output(const char *s) {
    size_t n, i = 0, t = 0;
    char *tmp, *out;

    if (!s) return xstrdup("");
    n = strlen(s);
    tmp = (char *)malloc(n + 1);
    if (!tmp) die("out of memory", NULL);

    while (i < n) {
        size_t ls = i, le;
        while (i < n && s[i] != '\n' && s[i] != '\r') i++;
        le = i;
        while (le > ls && (s[le - 1] == ' ' || s[le - 1] == '\t')) le--;
        if (le > ls) {
            memcpy(tmp + t, s + ls, le - ls);
            t += (le - ls);
        }
        tmp[t++] = '\n';
        if (i < n && s[i] == '\r') i++;
        if (i < n && s[i] == '\n') i++;
    }

    while (t > 0 && tmp[t - 1] == '\n') t--;
    out = (char *)malloc(t + 1);
    if (!out) die("out of memory", NULL);
    memcpy(out, tmp, t);
    out[t] = '\0';
    free(tmp);
    return out;
}

static char *make_excerpt(const char *s, size_t max_len) {
    char *norm = normalize_output(s);
    size_t n = strlen(norm);
    char *out;

    if (n <= max_len) return norm;
    if (max_len < 3) {
        free(norm);
        return xstrdup("");
    }
    out = (char *)malloc(max_len + 1);
    if (!out) die("out of memory", NULL);
    memcpy(out, norm, max_len);
    out[max_len] = '\0';
    out[max_len - 3] = '.';
    out[max_len - 2] = '.';
    out[max_len - 1] = '.';
    free(norm);
    return out;
}

static const char *signal_name_from_num(int sig) {
    switch (sig) {
        case SIGABRT: return "SIGABRT";
        case SIGALRM: return "SIGALRM";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGHUP: return "SIGHUP";
        case SIGILL: return "SIGILL";
        case SIGINT: return "SIGINT";
        case SIGKILL: return "SIGKILL";
        case SIGPIPE: return "SIGPIPE";
        case SIGQUIT: return "SIGQUIT";
        case SIGSEGV: return "SIGSEGV";
        case SIGTERM: return "SIGTERM";
        case SIGTRAP: return "SIGTRAP";
        default: return "UNKNOWN";
    }
}

static char *copy_with_ellipsis(const char *s, size_t n, size_t max_len) {
    char *out;
    if (!s || n == 0) return xstrdup("");
    if (n <= max_len) {
        out = (char *)malloc(n + 1);
        if (!out) die("out of memory", NULL);
        memcpy(out, s, n);
        out[n] = '\0';
        return out;
    }
    if (max_len < 3) return xstrdup("");
    out = (char *)malloc(max_len + 1);
    if (!out) die("out of memory", NULL);
    memcpy(out, s, max_len);
    out[max_len] = '\0';
    out[max_len - 3] = '.';
    out[max_len - 2] = '.';
    out[max_len - 1] = '.';
    return out;
}

static char *last_nonempty_line(const char *text, size_t max_len, size_t *nonempty_lines_out) {
    const char *line;
    const char *last_start = NULL;
    size_t last_len = 0;
    size_t nonempty = 0;

    if (!text) {
        if (nonempty_lines_out) *nonempty_lines_out = 0;
        return xstrdup("");
    }

    line = text;
    while (*line) {
        const char *raw_end = strchr(line, '\n');
        const char *start = line;
        const char *end;
        if (!raw_end) raw_end = line + strlen(line);
        end = raw_end;
        if (end > start && end[-1] == '\r') end--;
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        if (end > start) {
            nonempty++;
            last_start = start;
            last_len = (size_t)(end - start);
        }
        line = *raw_end ? raw_end + 1 : raw_end;
    }

    if (nonempty_lines_out) *nonempty_lines_out = nonempty;
    if (!last_start) return xstrdup("");
    return copy_with_ellipsis(last_start, last_len, max_len);
}

static int parse_phase_line_ms(const char *line_start,
                               const char *line_end,
                               const char *key,
                               double *out_ms) {
    size_t key_len = strlen(key);
    const char *start = line_start;
    const char *p;

    while (start < line_end && (*start == ' ' || *start == '\t')) start++;
    if ((size_t)(line_end - start) <= key_len) return 0;
    if (strncmp(start, key, key_len) != 0) return 0;

    p = start + key_len;
    while (p < line_end && !isdigit((unsigned char)*p) && *p != '-' && *p != '.') p++;
    if (p < line_end) {
        char *q = NULL;
        double v = strtod(p, &q);
        if (q && q > p) {
            *out_ms = v;
            return 1;
        }
    }
    return 0;
}

static void extract_timeout_phase_progress(const char *stdout_text,
                                           size_t *phase_count_out,
                                           char **last_phase_out,
                                           double *last_phase_ms_out) {
    static const char *k_phase_keys[] = {
        "File reading",
        "Src -> ASR",
        "ASR passes (total)",
        "ASR -> mod",
        "LLVM IR creation",
        "LLVM opt",
        "LLVM -> JIT",
        "JIT run",
        "Total time"
    };
    const char *line;
    size_t i;
    size_t phase_count = 0;
    const char *last_phase = NULL;
    double last_phase_ms = 0.0;

    if (phase_count_out) *phase_count_out = 0;
    if (last_phase_out) *last_phase_out = NULL;
    if (last_phase_ms_out) *last_phase_ms_out = 0.0;
    if (!stdout_text || !stdout_text[0]) return;

    line = stdout_text;
    while (*line) {
        const char *raw_end = strchr(line, '\n');
        const char *line_end;
        if (!raw_end) raw_end = line + strlen(line);
        line_end = raw_end;
        if (line_end > line && line_end[-1] == '\r') line_end--;
        for (i = 0; i < sizeof(k_phase_keys) / sizeof(k_phase_keys[0]); i++) {
            double v = 0.0;
            if (parse_phase_line_ms(line, line_end, k_phase_keys[i], &v)) {
                phase_count++;
                last_phase = k_phase_keys[i];
                last_phase_ms = v;
                break;
            }
        }
        line = *raw_end ? raw_end + 1 : raw_end;
    }

    if (phase_count_out) *phase_count_out = phase_count;
    if (last_phase_out && last_phase) *last_phase_out = xstrdup(last_phase);
    if (last_phase_ms_out) *last_phase_ms_out = last_phase_ms;
}

static void skip_diag_reset(skip_diag_t *d) {
    if (!d) return;
    free(d->stdout_text);
    free(d->stderr_text);
    free(d->stdout_excerpt);
    free(d->stderr_excerpt);
    free(d->last_stdout_line);
    free(d->last_stderr_line);
    free(d->time_report_last_phase);
    free(d->work_dir);
    free(d->stdout_log_path);
    free(d->stderr_log_path);
    memset(d, 0, sizeof(*d));
}

static void skip_diag_set_basic(skip_diag_t *d,
                                const char *reason,
                                const char *failing_side,
                                size_t iteration,
                                const char *stderr_text) {
    skip_diag_reset(d);
    d->reason = reason;
    d->failing_side = failing_side;
    d->iteration = iteration;
    d->stderr_text = xstrdup(stderr_text ? stderr_text : "");
    d->stderr_excerpt = make_excerpt(d->stderr_text, 256);
}

static void skip_diag_from_cmd(skip_diag_t *d,
                               const char *reason,
                               const char *failing_side,
                               size_t iteration,
                               const cmd_result_t *r,
                               int timeout_ms) {
    skip_diag_reset(d);
    d->reason = reason;
    d->failing_side = failing_side;
    d->iteration = iteration;
    d->timed_out = r ? r->timed_out : 0;
    d->timeout_ms = timeout_ms;
    if (r) {
        size_t stream_nonempty = 0;
        d->has_rc = 1;
        d->rc = r->rc;
        d->elapsed_ms = r->elapsed_ms;
        if (r->rc < 0) d->signal = -r->rc;
        d->stdout_text = xstrdup(r->stdout_text ? r->stdout_text : "");
        d->stderr_text = xstrdup(r->stderr_text ? r->stderr_text : "");
        d->stdout_bytes = strlen(d->stdout_text);
        d->stderr_bytes = strlen(d->stderr_text);
        d->stdout_excerpt = make_excerpt(d->stdout_text, 256);
        d->stderr_excerpt = make_excerpt(d->stderr_text, 256);
        d->last_stdout_line = last_nonempty_line(d->stdout_text, 160, &stream_nonempty);
        d->stdout_nonempty_lines = stream_nonempty;
        d->last_stderr_line = last_nonempty_line(d->stderr_text, 160, &stream_nonempty);
        d->stderr_nonempty_lines = stream_nonempty;
        if (d->timed_out) {
            extract_timeout_phase_progress(d->stdout_text,
                                           &d->time_report_phase_count,
                                           &d->time_report_last_phase,
                                           &d->time_report_last_phase_ms);
            d->timeout_silent = (d->stdout_nonempty_lines == 0 && d->stderr_nonempty_lines == 0);
        }
    }
}

static int write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (text && text[0] != '\0')
        fputs(text, f);
    fclose(f);
    return 0;
}

static void sanitize_token(const char *in, char *out, size_t out_sz) {
    size_t i = 0;
    if (out_sz == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (; in[i] && i + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_')
            out[i] = (char)c;
        else
            out[i] = '_';
    }
    out[i] = '\0';
}

static void maybe_write_failure_logs(const char *fail_log_dir, const char *name, skip_diag_t *diag) {
    char name_tok[256], reason_tok[128];
    char base[512];
    char *stdout_path, *stderr_path;
    int n;

    if (!fail_log_dir || !name || !diag) return;
    if (!diag->stdout_text && !diag->stderr_text) return;
    ensure_dir(fail_log_dir);

    sanitize_token(name, name_tok, sizeof(name_tok));
    sanitize_token(diag->reason ? diag->reason : "unknown", reason_tok, sizeof(reason_tok));
    n = snprintf(base, sizeof(base), "%s__%s__it%zu", name_tok, reason_tok, diag->iteration + 1);
    if (n < 0 || (size_t)n >= sizeof(base)) return;

    {
        char tmp[640];
        snprintf(tmp, sizeof(tmp), "%s.stdout.txt", base);
        stdout_path = path_join2(fail_log_dir, tmp);
    }
    {
        char tmp[640];
        snprintf(tmp, sizeof(tmp), "%s.stderr.txt", base);
        stderr_path = path_join2(fail_log_dir, tmp);
    }

    if (write_text_file(stdout_path, diag->stdout_text ? diag->stdout_text : "") == 0) {
        free(diag->stdout_log_path);
        diag->stdout_log_path = stdout_path;
    } else {
        free(stdout_path);
    }

    if (write_text_file(stderr_path, diag->stderr_text ? diag->stderr_text : "") == 0) {
        free(diag->stderr_log_path);
        diag->stderr_log_path = stderr_path;
    } else {
        free(stderr_path);
    }
}

static char *strip_ansi(const char *s) {
    size_t i, n = 0;
    char *out, *p;
    if (!s) return xstrdup("");
    for (i = 0; s[i]; i++) {
        if ((unsigned char)s[i] == 0x1b && s[i + 1] == '[') {
            i += 2;
            while (s[i] && (s[i] < '@' || s[i] > '~')) i++;
            if (!s[i]) break;
            continue;
        }
        n++;
    }
    out = (char *)malloc(n + 1);
    if (!out) die("out of memory", NULL);
    p = out;
    for (i = 0; s[i]; i++) {
        if ((unsigned char)s[i] == 0x1b && s[i + 1] == '[') {
            i += 2;
            while (s[i] && (s[i] < '@' || s[i] > '~')) i++;
            if (!s[i]) break;
            continue;
        }
        *p++ = s[i];
    }
    *p = '\0';
    return out;
}

static int parse_time_component_ms(const char *clean_text, const char *key, double *out_ms) {
    const char *line = clean_text;
    size_t key_len = strlen(key);
    while (*line) {
        const char *end = strchr(line, '\n');
        const char *start = line;
        if (!end) end = line + strlen(line);
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        if ((size_t)(end - start) > key_len && strncmp(start, key, key_len) == 0) {
            const char *p = start + key_len;
            while (p < end && !isdigit((unsigned char)*p) && *p != '-' && *p != '.') p++;
            if (p < end) {
                char *q = NULL;
                double v = strtod(p, &q);
                if (q && q > p) {
                    *out_ms = v;
                    return 1;
                }
            }
        }
        line = *end ? end + 1 : end;
    }
    return 0;
}

static double frontend_from_time_report(const time_report_t *r) {
    return r->file_read_ms + r->src_to_asr_ms + r->asr_passes_ms +
           r->asr_to_mod_ms + r->llvm_ir_ms + r->llvm_opt_ms;
}

static int parse_lfortran_time_report(const char *stdout_text, time_report_t *out) {
    char *clean = strip_ansi(stdout_text);
    double file_read = 0.0, src_to_asr = 0.0, asr_passes = 0.0;
    double asr_to_mod = 0.0, llvm_ir = 0.0, llvm_opt = 0.0;
    double llvm_to_jit = 0.0, jit_run = 0.0, total = 0.0;
    int ok =
        parse_time_component_ms(clean, "File reading", &file_read) &&
        parse_time_component_ms(clean, "Src -> ASR", &src_to_asr) &&
        parse_time_component_ms(clean, "ASR passes (total)", &asr_passes) &&
        parse_time_component_ms(clean, "ASR -> mod", &asr_to_mod) &&
        parse_time_component_ms(clean, "LLVM IR creation", &llvm_ir) &&
        parse_time_component_ms(clean, "LLVM opt", &llvm_opt) &&
        parse_time_component_ms(clean, "LLVM -> JIT", &llvm_to_jit) &&
        parse_time_component_ms(clean, "JIT run", &jit_run) &&
        parse_time_component_ms(clean, "Total time", &total);
    if (ok) {
        out->file_read_ms = file_read;
        out->src_to_asr_ms = src_to_asr;
        out->asr_passes_ms = asr_passes;
        out->asr_to_mod_ms = asr_to_mod;
        out->llvm_ir_ms = llvm_ir;
        out->llvm_opt_ms = llvm_opt;
        out->llvm_to_jit_ms = llvm_to_jit;
        out->jit_run_ms = jit_run;
        out->total_ms = total;
    }
    free(clean);
    return ok;
}

static void resolve_default_compat_artifacts(const char *bench_dir, char **compat_path, char **opts_path) {
    *compat_path = path_join2(bench_dir, "compat_ll.txt");
    *opts_path = path_join2(bench_dir, "compat_ll_options.jsonl");
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
                                   double front,
                                   double liric_llvm_ir,
                                   double llvm_llvm_ir,
                                   double wall_sp,
                                   double compile_sp,
                                   double run_sp) {
    char *en = json_escape(name ? name : "");
    fprintf(f,
            "{\"name\":\"%s\",\"status\":\"ok\",\"iters\":%zu,"
            "\"frontend_median_ms\":%.6f,"
            "\"liric_llvm_ir_median_ms\":%.6f,\"llvm_llvm_ir_median_ms\":%.6f,"
            "\"liric_wall_median_ms\":%.6f,\"llvm_wall_median_ms\":%.6f,"
            "\"liric_compile_median_ms\":%.6f,\"llvm_compile_median_ms\":%.6f,"
            "\"liric_run_median_ms\":%.6f,\"llvm_run_median_ms\":%.6f,"
            "\"wall_speedup\":%.6f,\"compile_speedup\":%.6f,\"run_speedup\":%.6f}\n",
            en, iters_done, front, liric_llvm_ir, llvm_llvm_ir,
            lw, ew, lc, ec, lr, er, wall_sp, compile_sp, run_sp);
    free(en);
}

static void write_json_nonzero_compat_row(FILE *f, const char *name, int rc) {
    char *en = json_escape(name ? name : "");
    fprintf(f,
            "{\"name\":\"%s\",\"status\":\"ok_nonzero_compat\",\"rc\":%d}\n",
            en, rc);
    free(en);
}

static void write_json_skip_diag_fields(FILE *f, const skip_diag_t *diag) {
    char *last_stdout = NULL;
    char *last_stderr = NULL;
    char *last_phase = NULL;
    if (!f || !diag) return;

    fprintf(f,
            ",\"elapsed_ms\":%.3f,\"timeout_ms\":%d,"
            "\"stdout_bytes\":%zu,\"stderr_bytes\":%zu,"
            "\"stdout_nonempty_lines\":%zu,\"stderr_nonempty_lines\":%zu",
            diag->elapsed_ms, diag->timeout_ms,
            diag->stdout_bytes, diag->stderr_bytes,
            diag->stdout_nonempty_lines, diag->stderr_nonempty_lines);

    if (diag->last_stdout_line && diag->last_stdout_line[0] != '\0') {
        last_stdout = json_escape(diag->last_stdout_line);
        fprintf(f, ",\"last_stdout_line\":\"%s\"", last_stdout);
    }
    if (diag->last_stderr_line && diag->last_stderr_line[0] != '\0') {
        last_stderr = json_escape(diag->last_stderr_line);
        fprintf(f, ",\"last_stderr_line\":\"%s\"", last_stderr);
    }

    if (diag->timed_out) {
        fprintf(f, ",\"timeout_silent\":%s,\"time_report_phase_count\":%zu",
                diag->timeout_silent ? "true" : "false",
                diag->time_report_phase_count);
        if (diag->time_report_last_phase && diag->time_report_last_phase[0] != '\0') {
            last_phase = json_escape(diag->time_report_last_phase);
            fprintf(f, ",\"time_report_last_phase\":\"%s\",\"time_report_last_phase_ms\":%.6f",
                    last_phase, diag->time_report_last_phase_ms);
        }
    }

    free(last_stdout);
    free(last_stderr);
    free(last_phase);
}

static void write_json_skip_row(FILE *f, const char *name, const skip_diag_t *diag) {
    char *en = json_escape(name ? name : "");
    char *er = json_escape((diag && diag->reason) ? diag->reason : "unknown");
    char *ef = json_escape((diag && diag->failing_side) ? diag->failing_side : "harness");
    char *esout = json_escape((diag && diag->stdout_excerpt) ? diag->stdout_excerpt : "");
    char *eserr = json_escape((diag && diag->stderr_excerpt) ? diag->stderr_excerpt : "");
    const char *sig_name = "UNKNOWN";
    if (diag && diag->signal > 0) sig_name = signal_name_from_num(diag->signal);
    fprintf(f,
            "{\"name\":\"%s\",\"status\":\"skipped\",\"reason\":\"%s\","
            "\"failing_side\":\"%s\",\"iter\":%zu,\"timed_out\":%s,"
            "\"rc\":%d,\"signal\":%d,\"signal_name\":\"%s\","
            "\"stdout_excerpt\":\"%s\",\"stderr_excerpt\":\"%s\"",
            en, er, ef,
            (diag ? diag->iteration + 1 : 0),
            (diag && diag->timed_out) ? "true" : "false",
            (diag && diag->has_rc) ? diag->rc : 0,
            (diag && diag->signal > 0) ? diag->signal : 0,
            sig_name, esout, eserr);
    if (diag) write_json_skip_diag_fields(f, diag);
    if (diag && diag->work_dir) {
        char *ework = json_escape(diag->work_dir);
        fprintf(f, ",\"work_dir\":\"%s\"", ework);
        free(ework);
    }
    if (diag && diag->stdout_log_path) {
        char *ep = json_escape(diag->stdout_log_path);
        fprintf(f, ",\"stdout_log\":\"%s\"", ep);
        free(ep);
    }
    if (diag && diag->stderr_log_path) {
        char *ep = json_escape(diag->stderr_log_path);
        fprintf(f, ",\"stderr_log\":\"%s\"", ep);
        free(ep);
    }
    fprintf(f, "}\n");
    free(en);
    free(er);
    free(ef);
    free(esout);
    free(eserr);
}

static void write_json_failure_detail_row(FILE *f, const char *name, const skip_diag_t *diag) {
    char *en, *er, *ef, *esout, *eserr;
    const char *sig_name = "UNKNOWN";
    if (!f) return;
    en = json_escape(name ? name : "");
    er = json_escape((diag && diag->reason) ? diag->reason : "unknown");
    ef = json_escape((diag && diag->failing_side) ? diag->failing_side : "harness");
    esout = json_escape((diag && diag->stdout_excerpt) ? diag->stdout_excerpt : "");
    eserr = json_escape((diag && diag->stderr_excerpt) ? diag->stderr_excerpt : "");
    if (diag && diag->signal > 0) sig_name = signal_name_from_num(diag->signal);

    fprintf(f,
            "{\"name\":\"%s\",\"reason\":\"%s\",\"failing_side\":\"%s\","
            "\"iter\":%zu,\"timed_out\":%s,\"rc\":%d,\"signal\":%d,\"signal_name\":\"%s\","
            "\"stdout_excerpt\":\"%s\",\"stderr_excerpt\":\"%s\"",
            en, er, ef,
            (diag ? diag->iteration + 1 : 0),
            (diag && diag->timed_out) ? "true" : "false",
            (diag && diag->has_rc) ? diag->rc : 0,
            (diag && diag->signal > 0) ? diag->signal : 0,
            sig_name, esout, eserr);
    if (diag) write_json_skip_diag_fields(f, diag);
    if (diag && diag->work_dir) {
        char *ework = json_escape(diag->work_dir);
        fprintf(f, ",\"work_dir\":\"%s\"", ework);
        free(ework);
    }
    if (diag && diag->stdout_log_path) {
        char *ep = json_escape(diag->stdout_log_path);
        fprintf(f, ",\"stdout_log\":\"%s\"", ep);
        free(ep);
    }
    if (diag && diag->stderr_log_path) {
        char *ep = json_escape(diag->stderr_log_path);
        fprintf(f, ",\"stderr_log\":\"%s\"", ep);
        free(ep);
    }
    fprintf(f, "}\n");

    free(en);
    free(er);
    free(ef);
    free(esout);
    free(eserr);
}

static int side_index(const char *side) {
    if (!side) return 2;
    if (strcmp(side, "llvm") == 0) return 0;
    if (strcmp(side, "liric") == 0) return 1;
    return 2;
}

#define SKIP_REASON_COUNT 13
static const char *k_skip_reasons[SKIP_REASON_COUNT] = {
    "workdir_create_failed",
    "source_missing",
    "llvm_jit_failed",
    "llvm_jit_verifier_pointee_mismatch",
    "llvm_jit_runtime_io_error",
    "llvm_jit_expected_nonzero_or_stop",
    "llvm_jit_timeout",
    "liric_jit_failed",
    "liric_jit_timeout",
    "llvm_jit_sigabrt",
    "llvm_jit_sigsegv",
    "liric_jit_sigabrt",
    "liric_jit_sigsegv"
};

static int skip_reason_index(const char *reason) {
    size_t i;
    if (!reason) return -1;
    for (i = 0; i < SKIP_REASON_COUNT; i++) {
        if (strcmp(k_skip_reasons[i], reason) == 0) return (int)i;
    }
    return -1;
}

static const char *classify_jit_failure_reason(int is_liric, int rc) {
    if (rc == -SIGABRT)
        return is_liric ? "liric_jit_sigabrt" : "llvm_jit_sigabrt";
    if (rc == -SIGSEGV)
        return is_liric ? "liric_jit_sigsegv" : "llvm_jit_sigsegv";
    return is_liric ? "liric_jit_failed" : "llvm_jit_failed";
}

static int text_has(const char *text, const char *needle) {
    return text && needle && strstr(text, needle) != NULL;
}

static int cmd_output_has(const cmd_result_t *r, const char *needle) {
    if (!r || !needle) return 0;
    return text_has(r->stdout_text, needle) || text_has(r->stderr_text, needle);
}

static const char *classify_llvm_failure_from_output(const cmd_result_t *r) {
    if (!r) return "llvm_jit_failed";
    if (r->rc == -SIGABRT || r->rc == -SIGSEGV)
        return classify_jit_failure_reason(0, r->rc);
    if (cmd_output_has(r, "explicit pointee type doesn't match operand's pointee type"))
        return "llvm_jit_verifier_pointee_mismatch";
    if (cmd_output_has(r, "Runtime error: File `") ||
        cmd_output_has(r, "Runtime error: End of file!") ||
        cmd_output_has(r, "Error: Failed to read") ||
        cmd_output_has(r, "Error: Invalid input for"))
        return "llvm_jit_runtime_io_error";
    if (cmd_output_has(r, "Error stop") ||
        cmd_output_has(r, "ERROR STOP") ||
        cmd_output_has(r, "\nSTOP") ||
        cmd_output_has(r, "\nSTOP ")) {
        return "llvm_jit_expected_nonzero_or_stop";
    }
    return "llvm_jit_failed";
}

static void usage(void) {
    printf("usage: bench_api [options]\n");
    printf("  --lfortran PATH      path to lfortran+LLVM binary (default: ../lfortran/build/src/bin/lfortran)\n");
    printf("  --lfortran-liric PATH path to lfortran+WITH_LIRIC binary (default: ../lfortran/build-liric/src/bin/lfortran)\n");
    printf("  --test-dir PATH      path to integration_tests/ dir\n");
    printf("  --bench-dir PATH     output directory (default: /tmp/liric_bench)\n");
    printf("  --compat-list PATH   compat list file (default: compat_ll.txt)\n");
    printf("  --options-jsonl PATH options jsonl file (default matches chosen compat list)\n");
    printf("  --iters N            iterations per test (default: 3)\n");
    printf("  --timeout N          per-command timeout in seconds (compat alias)\n");
    printf("  --timeout-ms N       per-command timeout in milliseconds (default: 3000)\n");
    printf("  --keep-fail-workdirs keep workdirs for skipped tests (default: off)\n");
    printf("  --fail-log-dir PATH  write detailed failure stdout/stderr logs here (default: <bench-dir>/fail_logs)\n");
    printf("  --fail-sample-limit N limit number of compat tests processed (default: all)\n");
    printf("  --min-completed N    fail if completed tests < N (default: 0)\n");
    printf("  --lookup-dispatch-share-pct N  optional profile-derived lookup/dispatch share percentage\n");
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;

    cfg.lfortran = "../lfortran/build/src/bin/lfortran";
    cfg.lfortran_liric = "../lfortran/build-liric/src/bin/lfortran";
    cfg.test_dir = "../lfortran/integration_tests";
    cfg.bench_dir = "/tmp/liric_bench";
    cfg.compat_list = NULL;
    cfg.options_jsonl = NULL;
    cfg.iters = 3;
    cfg.timeout_ms = 3000;
    cfg.keep_fail_workdirs = 0;
    cfg.fail_sample_limit = 0;
    cfg.fail_log_dir = NULL;
    cfg.min_completed = 0;
    cfg.lookup_dispatch_share_pct = -1.0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else if (strcmp(argv[i], "--lfortran") == 0 && i + 1 < argc) {
            cfg.lfortran = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-liric") == 0 && i + 1 < argc) {
            cfg.lfortran_liric = argv[++i];
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
            double sec = atof(argv[++i]);
            cfg.timeout_ms = (int)(sec * 1000.0);
            if (cfg.timeout_ms <= 0) cfg.timeout_ms = 3000;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            cfg.timeout_ms = atoi(argv[++i]);
            if (cfg.timeout_ms <= 0) cfg.timeout_ms = 3000;
        } else if (strcmp(argv[i], "--keep-fail-workdirs") == 0) {
            cfg.keep_fail_workdirs = 1;
        } else if (strcmp(argv[i], "--fail-log-dir") == 0 && i + 1 < argc) {
            cfg.fail_log_dir = argv[++i];
        } else if (strcmp(argv[i], "--fail-sample-limit") == 0 && i + 1 < argc) {
            cfg.fail_sample_limit = atoi(argv[++i]);
            if (cfg.fail_sample_limit < 0) cfg.fail_sample_limit = 0;
        } else if (strcmp(argv[i], "--min-completed") == 0 && i + 1 < argc) {
            cfg.min_completed = atoi(argv[++i]);
            if (cfg.min_completed < 0) cfg.min_completed = 0;
        } else if (strcmp(argv[i], "--lookup-dispatch-share-pct") == 0 && i + 1 < argc) {
            cfg.lookup_dispatch_share_pct = atof(argv[++i]);
            if (cfg.lookup_dispatch_share_pct < 0.0)
                cfg.lookup_dispatch_share_pct = -1.0;
        } else {
            die("unknown argument", argv[i]);
        }
    }

    if (!file_exists(cfg.lfortran)) die("lfortran (LLVM) not found", cfg.lfortran);
    if (!file_exists(cfg.lfortran_liric)) die("lfortran (WITH_LIRIC) not found", cfg.lfortran_liric);

    cfg.lfortran = to_abs_path(cfg.lfortran);
    cfg.lfortran_liric = to_abs_path(cfg.lfortran_liric);
    cfg.test_dir = to_abs_path(cfg.test_dir);
    cfg.bench_dir = to_abs_path(cfg.bench_dir);
    if (cfg.compat_list) cfg.compat_list = to_abs_path(cfg.compat_list);
    if (cfg.options_jsonl) cfg.options_jsonl = to_abs_path(cfg.options_jsonl);
    if (cfg.fail_log_dir) cfg.fail_log_dir = to_abs_path(cfg.fail_log_dir);

    return cfg;
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    char *compat_path = NULL;
    char *opts_path = NULL;
    char *fail_log_dir = NULL;

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

    char *jsonl_path = path_join2(cfg.bench_dir, "bench_api.jsonl");
    char *summary_path = path_join2(cfg.bench_dir, "bench_api_summary.json");
    char *fail_jsonl_path = path_join2(cfg.bench_dir, "bench_api_failures.jsonl");
    char *fail_summary_path = path_join2(cfg.bench_dir, "bench_api_fail_summary.json");
    FILE *f;
    strlist_t tests;
    optlist_t opts;
    rowlist_t rows;
    size_t skip_reason_counts[SKIP_REASON_COUNT];
    size_t skip_side_counts[3] = {0, 0, 0}; // llvm, liric, harness
    size_t compat_nonzero_completed = 0;
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
            if (n > 0) {
                strlist_push(&tests, line);
            }
        }
    }
    fclose(f);

    validate_compat_sources(&tests, cfg.test_dir, compat_path);
    if (cfg.fail_sample_limit > 0 && tests.n > (size_t)cfg.fail_sample_limit)
        strlist_truncate(&tests, (size_t)cfg.fail_sample_limit);

    opts = parse_options_jsonl(opts_path);
    ensure_dir(cfg.bench_dir);
    if (cfg.fail_log_dir)
        fail_log_dir = xstrdup(cfg.fail_log_dir);
    else
        fail_log_dir = path_join2(cfg.bench_dir, "fail_logs");

    printf("Benchmarking %zu tests, %d iterations each\n", tests.n, cfg.iters);
    printf("  lfortran LLVM:  %s\n", cfg.lfortran);
    printf("  lfortran liric: %s\n", cfg.lfortran_liric);
    printf("  test_dir:      %s\n", cfg.test_dir);
    printf("  bench_dir:     %s\n", cfg.bench_dir);
    printf("  compat_list:   %s\n", compat_path);
    printf("  options_jsonl: %s\n", opts_path);
    if (cfg.fail_sample_limit > 0) {
        printf("  fail_sample_limit: %d\n", cfg.fail_sample_limit);
    }
    printf("  fail_log_dir:   %s\n", fail_log_dir);
    printf("  keep_fail_workdirs: %s\n", cfg.keep_fail_workdirs ? "on" : "off");
    printf("  min_completed: %d\n", cfg.min_completed);
    if (cfg.lookup_dispatch_share_pct >= 0.0) {
        printf("  lookup_dispatch_share_pct: %.3f\n", cfg.lookup_dispatch_share_pct);
    }

    {
        FILE *jf = fopen(jsonl_path, "w");
        FILE *ff = fopen(fail_jsonl_path, "w");
        if (!jf) die("failed to open output", jsonl_path);
        if (!ff) die("failed to open output", fail_jsonl_path);

        for (i = 0; i < tests.n; i++) {
            const char *name = tests.items[i];
            const char *test_opts = optlist_find(&opts, name);
            strlist_t opt_toks;
            skip_diag_t skip_diag;
            char *source_path = NULL;
            size_t it;
            double *liric_wall = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_wall = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_compile = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_run = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_compile = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_run = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *frontend = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *liric_llvm_ir = (double *)calloc((size_t)cfg.iters, sizeof(double));
            double *llvm_llvm_ir = (double *)calloc((size_t)cfg.iters, sizeof(double));
            size_t ok_n = 0;
            const char *skip_reason = NULL;
            int nonzero_compat = 0;
            int nonzero_compat_rc = 0;
            char work_tpl[PATH_MAX];
            const char *work_dir = NULL;
            const char *failure_work_dir = NULL;
            int keep_work_dir = 0;
            memset(&skip_diag, 0, sizeof(skip_diag));

            strlist_init(&opt_toks);
            {
                int n = snprintf(work_tpl, sizeof(work_tpl), "%s/%s", cfg.bench_dir, "work_api_jit_XXXXXX");
                if (n < 0 || (size_t)n >= sizeof(work_tpl)) {
                    skip_reason = "workdir_create_failed";
                    skip_diag_set_basic(&skip_diag, skip_reason, "harness", 0,
                                        "workdir template exceeded PATH_MAX");
                    goto skip_test;
                }
            }
            if (!mkdtemp(work_tpl)) {
                skip_reason = "workdir_create_failed";
                skip_diag_set_basic(&skip_diag, skip_reason, "harness", 0, strerror(errno));
                goto skip_test;
            }
            work_dir = work_tpl;
            failure_work_dir = work_dir;

            opt_toks = tokenize_options(test_opts);

            {
                char fname[512];
                snprintf(fname, sizeof(fname), "%s.f90", name);
                source_path = path_join2(cfg.test_dir, fname);
            }

            if (!file_exists(source_path)) {
                skip_reason = "source_missing";
                skip_diag_set_basic(&skip_diag, skip_reason, "harness", 0, "source file missing");
                goto skip_test;
            }

            for (it = 0; it < (size_t)cfg.iters; it++) {
                cmd_result_t llvm_r, liric_r;
                time_report_t llvm_time;
                time_report_t liric_time;
                double llvm_front = 0.0, llvm_compile_ms = 0.0, llvm_run_ms = 0.0, llvm_wall_ms = 0.0;
                double liric_front = 0.0, liric_compile_ms = 0.0, liric_run_ms = 0.0, liric_wall_ms = 0.0;
                const char *attempt_work_dir = work_dir;
                const char *extra_retry_opt = NULL;
                int retried_test_dir = 0;
                int retried_fast = 0;
                int run_success = 0;

                for (;;) {
                    const char *llvm_reason = NULL;
                    int same_nonzero = 0;
                    failure_work_dir = attempt_work_dir;

                    llvm_r = run_lfortran_jit_cmd(cfg.lfortran,
                                                  &opt_toks,
                                                  extra_retry_opt,
                                                  source_path,
                                                  cfg.timeout_ms,
                                                  attempt_work_dir);
                    if (llvm_r.timed_out) {
                        skip_reason = "llvm_jit_timeout";
                        skip_diag_from_cmd(&skip_diag, skip_reason, "llvm", it, &llvm_r, cfg.timeout_ms);
                        free_cmd_result(&llvm_r);
                        break;
                    }

                    liric_r = run_lfortran_jit_cmd(cfg.lfortran_liric,
                                                   &opt_toks,
                                                   extra_retry_opt,
                                                   source_path,
                                                   cfg.timeout_ms,
                                                   attempt_work_dir);
                    if (liric_r.timed_out) {
                        skip_reason = "liric_jit_timeout";
                        skip_diag_from_cmd(&skip_diag, skip_reason, "liric", it, &liric_r, cfg.timeout_ms);
                        free_cmd_result(&llvm_r);
                        free_cmd_result(&liric_r);
                        break;
                    }

                    if (llvm_r.rc == 0 && liric_r.rc == 0) {
                        run_success = 1;
                        break;
                    }

                    if (llvm_r.rc != 0)
                        llvm_reason = classify_llvm_failure_from_output(&llvm_r);

                    if (!retried_test_dir &&
                        llvm_r.rc != 0 &&
                        llvm_reason &&
                        strcmp(llvm_reason, "llvm_jit_runtime_io_error") == 0 &&
                        strcmp(attempt_work_dir, cfg.test_dir) != 0) {
                        retried_test_dir = 1;
                        attempt_work_dir = cfg.test_dir;
                        free_cmd_result(&llvm_r);
                        free_cmd_result(&liric_r);
                        continue;
                    }

                    if (!retried_fast &&
                        llvm_r.rc != 0 &&
                        llvm_reason &&
                        strcmp(llvm_reason, "llvm_jit_verifier_pointee_mismatch") == 0 &&
                        !strlist_contains_exact(&opt_toks, "--fast")) {
                        retried_fast = 1;
                        extra_retry_opt = "--fast";
                        free_cmd_result(&llvm_r);
                        free_cmd_result(&liric_r);
                        continue;
                    }

                    if (llvm_r.rc != 0 && liric_r.rc != 0 && llvm_r.rc == liric_r.rc) {
                        char *llvm_out = normalize_output(llvm_r.stdout_text);
                        char *liric_out = normalize_output(liric_r.stdout_text);
                        char *llvm_err = normalize_output(llvm_r.stderr_text);
                        char *liric_err = normalize_output(liric_r.stderr_text);
                        same_nonzero = (strcmp(llvm_out, liric_out) == 0) &&
                                       (strcmp(llvm_err, liric_err) == 0);
                        free(llvm_out);
                        free(liric_out);
                        free(llvm_err);
                        free(liric_err);
                    }
                    if (same_nonzero) {
                        nonzero_compat = 1;
                        nonzero_compat_rc = llvm_r.rc;
                    } else if (llvm_r.rc != 0) {
                        skip_reason = llvm_reason ? llvm_reason : classify_jit_failure_reason(0, llvm_r.rc);
                        skip_diag_from_cmd(&skip_diag, skip_reason, "llvm", it, &llvm_r, cfg.timeout_ms);
                    } else {
                        skip_reason = classify_jit_failure_reason(1, liric_r.rc);
                        skip_diag_from_cmd(&skip_diag, skip_reason, "liric", it, &liric_r, cfg.timeout_ms);
                    }
                    free_cmd_result(&llvm_r);
                    free_cmd_result(&liric_r);
                    break;
                }

                if (!run_success) break;

                memset(&llvm_time, 0, sizeof(llvm_time));
                if (!parse_lfortran_time_report(llvm_r.stdout_text, &llvm_time)) {
                    skip_reason = "llvm_jit_failed";
                    skip_diag_from_cmd(&skip_diag, skip_reason, "llvm", it, &llvm_r, cfg.timeout_ms);
                    free_cmd_result(&llvm_r);
                    free_cmd_result(&liric_r);
                    break;
                }
                memset(&liric_time, 0, sizeof(liric_time));
                if (!parse_lfortran_time_report(liric_r.stdout_text, &liric_time)) {
                    skip_reason = "liric_jit_failed";
                    skip_diag_from_cmd(&skip_diag, skip_reason, "liric", it, &liric_r, cfg.timeout_ms);
                    free_cmd_result(&llvm_r);
                    free_cmd_result(&liric_r);
                    break;
                }
                free_cmd_result(&llvm_r);
                free_cmd_result(&liric_r);

                llvm_front = frontend_from_time_report(&llvm_time);
                llvm_compile_ms = llvm_time.llvm_to_jit_ms;
                llvm_run_ms = llvm_time.jit_run_ms;
                llvm_wall_ms = llvm_time.total_ms;
                liric_front = frontend_from_time_report(&liric_time);
                liric_compile_ms = liric_time.llvm_to_jit_ms;
                liric_run_ms = liric_time.jit_run_ms;
                liric_wall_ms = liric_time.total_ms;

                frontend[ok_n] = 0.5 * (llvm_front + liric_front);
                llvm_compile[ok_n] = llvm_compile_ms;
                llvm_run[ok_n] = llvm_run_ms;
                llvm_wall[ok_n] = llvm_wall_ms;
                liric_compile[ok_n] = liric_compile_ms;
                liric_run[ok_n] = liric_run_ms;
                liric_wall[ok_n] = liric_wall_ms;
                llvm_llvm_ir[ok_n] = llvm_time.llvm_ir_ms;
                liric_llvm_ir[ok_n] = liric_time.llvm_ir_ms;
                ok_n++;
            }

            if (ok_n == 0) {
                if (nonzero_compat) {
                    compat_nonzero_completed++;
                    write_json_nonzero_compat_row(jf, name, nonzero_compat_rc);
                    printf("  [%zu/%zu] %s: compatible non-zero rc (%d)\n",
                           i + 1, tests.n, name, nonzero_compat_rc);
                    goto next_test;
                }
                if (!skip_reason) skip_reason = "llvm_jit_failed";
                if (!skip_diag.reason)
                    skip_diag_set_basic(&skip_diag, skip_reason, "harness", 0, "unknown failure");
                goto skip_test;
            }

            {
                double lw = median(liric_wall, ok_n);
                double ew = median(llvm_wall, ok_n);
                double lc = median(liric_compile, ok_n);
                double ec = median(llvm_compile, ok_n);
                double lr = median(liric_run, ok_n);
                double er = median(llvm_run, ok_n);
                double fm = median(frontend, ok_n);
                double liric_ir = median(liric_llvm_ir, ok_n);
                double llvm_ir = median(llvm_llvm_ir, ok_n);
                double wall_sp = lw > 0 ? ew / lw : 0.0;
                double compile_sp = lc > 0 ? ec / lc : 0.0;
                double run_sp = lr > 0 ? er / lr : 0.0;
                double ir_sp = liric_ir > 0 ? llvm_ir / liric_ir : 0.0;
                row_t row;

                row.name = xstrdup(name);
                row.liric_wall_ms = lw;
                row.llvm_wall_ms = ew;
                row.liric_compile_ms = lc;
                row.liric_run_ms = lr;
                row.llvm_compile_ms = ec;
                row.llvm_run_ms = er;
                row.frontend_ms = fm;
                row.liric_llvm_ir_ms = liric_ir;
                row.llvm_llvm_ir_ms = llvm_ir;
                rowlist_push(&rows, row);

                write_json_success_row(jf, name, ok_n, lw, ew, lc, ec, lr, er, fm,
                                       liric_ir, llvm_ir, wall_sp, compile_sp, run_sp);
                printf("  [%zu/%zu] %s: wall %.2fms vs %.2fms (%.2fx), ir %.2fms vs %.2fms (%.2fx), jit %.2fms vs %.2fms (%.2fx)\n",
                       i + 1, tests.n, name, lw, ew, wall_sp, liric_ir, llvm_ir, ir_sp, lc, ec, compile_sp);
            }
            goto next_test;

skip_test:
            {
                int idx = skip_reason_index(skip_reason);
                if (idx >= 0) skip_reason_counts[(size_t)idx]++;
            }
            if (!skip_diag.reason)
                skip_diag_set_basic(&skip_diag, skip_reason ? skip_reason : "unknown", "harness", 0, "");
            maybe_write_failure_logs(fail_log_dir, name, &skip_diag);
            if (!skip_diag.work_dir && failure_work_dir)
                skip_diag.work_dir = xstrdup(failure_work_dir);
            if (cfg.keep_fail_workdirs &&
                work_dir &&
                failure_work_dir &&
                strcmp(failure_work_dir, work_dir) == 0) {
                keep_work_dir = 1;
                free(skip_diag.work_dir);
                skip_diag.work_dir = xstrdup(work_dir);
            }
            skip_side_counts[(size_t)side_index(skip_diag.failing_side)]++;
            write_json_skip_row(jf, name, &skip_diag);
            write_json_failure_detail_row(ff, name, &skip_diag);
            printf("  [%zu/%zu] %s: skipped (%s)\n", i + 1, tests.n, name, skip_reason ? skip_reason : "unknown");

next_test:
            if (work_dir && !keep_work_dir) remove_tree(work_dir);
            free(source_path);
            free(liric_wall);
            free(llvm_wall);
            free(liric_compile);
            free(liric_run);
            free(llvm_compile);
            free(llvm_run);
            free(frontend);
            free(liric_llvm_ir);
            free(llvm_llvm_ir);
            strlist_free(&opt_toks);
            skip_diag_reset(&skip_diag);
        }

        fclose(jf);
        fclose(ff);
    }

    {
        double tracker_liric_llvm_ir_avg_median = 0.0;
        double tracker_llvm_llvm_ir_avg_median = 0.0;
        double tracker_liric_llvm_to_jit_avg_median = 0.0;
        double tracker_llvm_llvm_to_jit_avg_median = 0.0;
        double tracker_run_speedup_avg = 0.0;
        double tracker_run_speedup_min = 0.0;
        int tracker_has_data = 0;
        int tracker_llvm_ir_creation_met = 0;
        int tracker_llvm_to_jit_met = 0;
        int tracker_run_speedup_avg_met = 0;
        int tracker_run_speedup_each_met = 0;
        int tracker_lookup_dispatch_available = (cfg.lookup_dispatch_share_pct >= 0.0);
        int tracker_lookup_dispatch_met = 0;
        int tracker_all_targets_met = 0;

        if (rows.n > 0) {
        double *lw = (double *)malloc(rows.n * sizeof(double));
        double *ew = (double *)malloc(rows.n * sizeof(double));
        double *lc = (double *)malloc(rows.n * sizeof(double));
        double *ec = (double *)malloc(rows.n * sizeof(double));
        double *lr = (double *)malloc(rows.n * sizeof(double));
        double *er = (double *)malloc(rows.n * sizeof(double));
        double *fm = (double *)malloc(rows.n * sizeof(double));
        double *li = (double *)malloc(rows.n * sizeof(double));
        double *ei = (double *)malloc(rows.n * sizeof(double));
        double *ir_sp = (double *)malloc(rows.n * sizeof(double));
        double *wall_sp = (double *)malloc(rows.n * sizeof(double));
        double *compile_sp = (double *)malloc(rows.n * sizeof(double));
        double *run_sp = (double *)malloc(rows.n * sizeof(double));
        size_t j;
        size_t wall_faster = 0;
        size_t compile_faster = 0;
        size_t run_faster = 0;
        size_t ir_faster = 0;
        double sum_lw = 0.0, sum_ew = 0.0;
        double sum_li = 0.0, sum_ei = 0.0;
        double sum_lc = 0.0, sum_ec = 0.0;
        double sum_lr = 0.0, sum_er = 0.0;
        double sum_fm = 0.0;
        double sum_run_sp = 0.0;

        for (j = 0; j < rows.n; j++) {
            lw[j] = rows.items[j].liric_wall_ms;
            ew[j] = rows.items[j].llvm_wall_ms;
            lc[j] = rows.items[j].liric_compile_ms;
            ec[j] = rows.items[j].llvm_compile_ms;
            lr[j] = rows.items[j].liric_run_ms;
            er[j] = rows.items[j].llvm_run_ms;
            fm[j] = rows.items[j].frontend_ms;
            li[j] = rows.items[j].liric_llvm_ir_ms;
            ei[j] = rows.items[j].llvm_llvm_ir_ms;
            ir_sp[j] = li[j] > 0 ? ei[j] / li[j] : 0.0;
            wall_sp[j] = lw[j] > 0 ? ew[j] / lw[j] : 0.0;
            compile_sp[j] = lc[j] > 0 ? ec[j] / lc[j] : 0.0;
            run_sp[j] = lr[j] > 0 ? er[j] / lr[j] : 0.0;
            if (ir_sp[j] > 1.0) ir_faster++;
            if (wall_sp[j] > 1.0) wall_faster++;
            if (compile_sp[j] > 1.0) compile_faster++;
            if (run_sp[j] > 1.0) run_faster++;
            sum_li += li[j];
            sum_ei += ei[j];
            sum_lw += lw[j];
            sum_ew += ew[j];
            sum_lc += lc[j];
            sum_ec += ec[j];
            sum_lr += lr[j];
            sum_er += er[j];
            sum_fm += fm[j];
            sum_run_sp += run_sp[j];
        }

        printf("\n========================================================================\n");
        printf("  API JIT mode: Fortran frontend + LLVM JIT vs Fortran frontend + liric JIT\n");
        printf("  %zu tests, %d iterations each\n", rows.n, cfg.iters);
        printf("========================================================================\n");

        printf("\n  FRONTEND (common to both)\n");
        printf("  Median:    %.3f ms\n", median(fm, rows.n));
        printf("  Aggregate: %.0f ms\n", sum_fm);

        printf("\n  PHASE: LLVM IR CREATION\n");
        printf("  Median:    liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(li, rows.n), median(ei, rows.n), median(ir_sp, rows.n));
        printf("  Aggregate: %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_li, sum_ei, (sum_li > 0 ? sum_ei / sum_li : 0.0));
        printf("  Faster:    %zu/%zu (%.1f%%)\n",
               ir_faster, rows.n, 100.0 * (double)ir_faster / (double)rows.n);

        printf("\n  WALL-CLOCK (frontend + jit-materialize + exec)\n");
        printf("  Median:    liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(lw, rows.n), median(ew, rows.n), median(wall_sp, rows.n));
        printf("  Aggregate: %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lw, sum_ew, (sum_lw > 0 ? sum_ew / sum_lw : 0.0));
        printf("  P90/P95:   %.2fx / %.2fx\n", percentile(wall_sp, rows.n, 90), percentile(wall_sp, rows.n, 95));
        printf("  Faster:    %zu/%zu (%.1f%%)\n", wall_faster, rows.n, 100.0 * (double)wall_faster / (double)rows.n);

        printf("\n  JIT MATERIALIZATION (LLVM -> JIT)\n");
        printf("  Median:    liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(lc, rows.n), median(ec, rows.n), median(compile_sp, rows.n));
        printf("  Aggregate: %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lc, sum_ec, (sum_lc > 0 ? sum_ec / sum_lc : 0.0));
        printf("  Faster:    %zu/%zu (%.1f%%)\n",
               compile_faster, rows.n, 100.0 * (double)compile_faster / (double)rows.n);

        printf("\n  EXECUTION (entry invocation only)\n");
        printf("  Median:    liric %.3f ms, llvm %.3f ms, speedup %.2fx\n",
               median(lr, rows.n), median(er, rows.n), median(run_sp, rows.n));
        printf("  Aggregate: %.0f ms vs %.0f ms, speedup %.2fx\n",
               sum_lr, sum_er, (sum_lr > 0 ? sum_er / sum_lr : 0.0));
        printf("  Faster:    %zu/%zu (%.1f%%)\n",
               run_faster, rows.n, 100.0 * (double)run_faster / (double)rows.n);

        tracker_has_data = 1;
        tracker_liric_llvm_ir_avg_median = sum_li / (double)rows.n;
        tracker_llvm_llvm_ir_avg_median = sum_ei / (double)rows.n;
        tracker_liric_llvm_to_jit_avg_median = sum_lc / (double)rows.n;
        tracker_llvm_llvm_to_jit_avg_median = sum_ec / (double)rows.n;
        tracker_run_speedup_avg = sum_run_sp / (double)rows.n;
        tracker_run_speedup_min = run_sp[0];
        for (j = 1; j < rows.n; j++) {
            if (run_sp[j] < tracker_run_speedup_min)
                tracker_run_speedup_min = run_sp[j];
        }
        tracker_llvm_ir_creation_met =
            tracker_liric_llvm_ir_avg_median <= TRACKER_TARGET_LLVM_IR_CREATION_MS;
        tracker_llvm_to_jit_met =
            tracker_liric_llvm_to_jit_avg_median <= TRACKER_TARGET_LLVM_TO_JIT_MS;
        tracker_run_speedup_avg_met =
            tracker_run_speedup_avg >= TRACKER_TARGET_RUN_SPEEDUP_AVG;
        tracker_run_speedup_each_met =
            tracker_run_speedup_min >= TRACKER_TARGET_RUN_SPEEDUP_MIN;
        if (tracker_lookup_dispatch_available) {
            tracker_lookup_dispatch_met =
                cfg.lookup_dispatch_share_pct <= TRACKER_TARGET_LOOKUP_DISPATCH_PCT;
        }
        tracker_all_targets_met = tracker_llvm_ir_creation_met &&
                                  tracker_llvm_to_jit_met &&
                                  tracker_run_speedup_avg_met &&
                                  tracker_run_speedup_each_met &&
                                  tracker_lookup_dispatch_available &&
                                  tracker_lookup_dispatch_met;

        printf("\n  PHASE TRACKER (#233)\n");
        printf("  LLVM IR creation avg median: %.3f ms (target <= %.3f ms): %s\n",
               tracker_liric_llvm_ir_avg_median, TRACKER_TARGET_LLVM_IR_CREATION_MS,
               tracker_llvm_ir_creation_met ? "met" : "not met");
        printf("  LLVM -> JIT avg median:      %.3f ms (target <= %.3f ms): %s\n",
               tracker_liric_llvm_to_jit_avg_median, TRACKER_TARGET_LLVM_TO_JIT_MS,
               tracker_llvm_to_jit_met ? "met" : "not met");
        printf("  JIT run speedup avg/min:     %.2fx / %.2fx (targets >= %.2fx avg, >= %.2fx each): %s\n",
               tracker_run_speedup_avg, tracker_run_speedup_min,
               TRACKER_TARGET_RUN_SPEEDUP_AVG, TRACKER_TARGET_RUN_SPEEDUP_MIN,
               (tracker_run_speedup_avg_met && tracker_run_speedup_each_met) ? "met" : "not met");
        if (tracker_lookup_dispatch_available) {
            printf("  Lookup/dispatch share:       %.3f%% (target <= %.2f%%): %s\n",
                   cfg.lookup_dispatch_share_pct, TRACKER_TARGET_LOOKUP_DISPATCH_PCT,
                   tracker_lookup_dispatch_met ? "met" : "not met");
        } else {
            printf("  Lookup/dispatch share:       not provided (pass --lookup-dispatch-share-pct)\n");
        }

        printf("\n  Results: %s\n", jsonl_path);

        free(lw);
        free(ew);
        free(lc);
        free(ec);
        free(lr);
        free(er);
        free(fm);
        free(li);
        free(ei);
        free(ir_sp);
        free(wall_sp);
        free(compile_sp);
        free(run_sp);
        }

        size_t attempted = tests.n;
        size_t completed_timed = rows.n;
        size_t completed = completed_timed + compat_nonzero_completed;
        size_t skipped = (attempted >= completed) ? (attempted - completed) : 0;
        FILE *sf = fopen(summary_path, "w");
        FILE *fsf = fopen(fail_summary_path, "w");
        if (!sf) die("failed to open output", summary_path);
        if (!fsf) die("failed to open output", fail_summary_path);

        fprintf(sf, "{\n");
        fprintf(sf, "  \"attempted\": %zu,\n", attempted);
        fprintf(sf, "  \"completed\": %zu,\n", completed);
        fprintf(sf, "  \"completed_timed\": %zu,\n", completed_timed);
        fprintf(sf, "  \"completed_nonzero_compat\": %zu,\n", compat_nonzero_completed);
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
        {
            char *efj = json_escape(fail_jsonl_path);
            char *efl = json_escape(fail_log_dir);
            fprintf(sf, "  \"failure_jsonl\": \"%s\",\n", efj);
            fprintf(sf, "  \"failure_log_dir\": \"%s\",\n", efl);
            fprintf(sf, "  \"keep_fail_workdirs\": %s,\n", cfg.keep_fail_workdirs ? "true" : "false");
            free(efj);
            free(efl);
        }
        free(ec);
        free(eo);
        }
        fprintf(sf, "  \"phase_tracker\": {\n");
        fprintf(sf, "    \"has_data\": %s,\n", tracker_has_data ? "true" : "false");
        fprintf(sf, "    \"targets\": {\n");
        fprintf(sf, "      \"llvm_ir_creation_target_ms\": %.6f,\n", TRACKER_TARGET_LLVM_IR_CREATION_MS);
        fprintf(sf, "      \"llvm_to_jit_target_ms\": %.6f,\n", TRACKER_TARGET_LLVM_TO_JIT_MS);
        fprintf(sf, "      \"run_speedup_avg_target\": %.6f,\n", TRACKER_TARGET_RUN_SPEEDUP_AVG);
        fprintf(sf, "      \"run_speedup_each_target\": %.6f,\n", TRACKER_TARGET_RUN_SPEEDUP_MIN);
        fprintf(sf, "      \"lookup_dispatch_target_pct\": %.6f\n", TRACKER_TARGET_LOOKUP_DISPATCH_PCT);
        fprintf(sf, "    },\n");
        fprintf(sf, "    \"metrics\": {\n");
        fprintf(sf, "      \"liric_llvm_ir_avg_median_ms\": %.6f,\n", tracker_liric_llvm_ir_avg_median);
        fprintf(sf, "      \"llvm_llvm_ir_avg_median_ms\": %.6f,\n", tracker_llvm_llvm_ir_avg_median);
        fprintf(sf, "      \"liric_llvm_to_jit_avg_median_ms\": %.6f,\n", tracker_liric_llvm_to_jit_avg_median);
        fprintf(sf, "      \"llvm_llvm_to_jit_avg_median_ms\": %.6f,\n", tracker_llvm_llvm_to_jit_avg_median);
        fprintf(sf, "      \"run_speedup_avg\": %.6f,\n", tracker_run_speedup_avg);
        fprintf(sf, "      \"run_speedup_min\": %.6f,\n", tracker_run_speedup_min);
        if (tracker_lookup_dispatch_available)
            fprintf(sf, "      \"lookup_dispatch_share_pct\": %.6f\n", cfg.lookup_dispatch_share_pct);
        else
            fprintf(sf, "      \"lookup_dispatch_share_pct\": null\n");
        fprintf(sf, "    },\n");
        fprintf(sf, "    \"criteria\": {\n");
        fprintf(sf, "      \"llvm_ir_creation_met\": %s,\n", tracker_llvm_ir_creation_met ? "true" : "false");
        fprintf(sf, "      \"llvm_to_jit_met\": %s,\n", tracker_llvm_to_jit_met ? "true" : "false");
        fprintf(sf, "      \"run_speedup_avg_met\": %s,\n", tracker_run_speedup_avg_met ? "true" : "false");
        fprintf(sf, "      \"run_speedup_each_met\": %s,\n", tracker_run_speedup_each_met ? "true" : "false");
        if (tracker_lookup_dispatch_available)
            fprintf(sf, "      \"lookup_dispatch_met\": %s\n", tracker_lookup_dispatch_met ? "true" : "false");
        else
            fprintf(sf, "      \"lookup_dispatch_met\": null\n");
        fprintf(sf, "    },\n");
        fprintf(sf, "    \"all_targets_met\": %s\n", tracker_all_targets_met ? "true" : "false");
        fprintf(sf, "  },\n");
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

        fprintf(fsf, "{\n");
        fprintf(fsf, "  \"attempted\": %zu,\n", attempted);
        fprintf(fsf, "  \"completed\": %zu,\n", completed);
        fprintf(fsf, "  \"failed\": %zu,\n", skipped);
        {
            char *efj = json_escape(fail_jsonl_path);
            char *efd = json_escape(fail_log_dir);
            fprintf(fsf, "  \"failure_jsonl\": \"%s\",\n", efj);
            fprintf(fsf, "  \"failure_log_dir\": \"%s\",\n", efd);
            free(efj);
            free(efd);
        }
        fprintf(fsf, "  \"failing_side_counts\": {\n");
        fprintf(fsf, "    \"llvm\": %zu,\n", skip_side_counts[0]);
        fprintf(fsf, "    \"liric\": %zu,\n", skip_side_counts[1]);
        fprintf(fsf, "    \"harness\": %zu\n", skip_side_counts[2]);
        fprintf(fsf, "  },\n");
        fprintf(fsf, "  \"skip_reasons\": {\n");
        for (i = 0; i < SKIP_REASON_COUNT; i++) {
            fprintf(fsf, "    \"%s\": %zu%s\n",
                    k_skip_reasons[i],
                    skip_reason_counts[i],
                    (i + 1 == SKIP_REASON_COUNT) ? "" : ",");
        }
        fprintf(fsf, "  }\n");
        fprintf(fsf, "}\n");
        fclose(fsf);

        printf("\n  Accounting: attempted=%zu completed=%zu skipped=%zu\n",
               attempted, completed, skipped);
        if (compat_nonzero_completed > 0) {
            printf("    completed_nonzero_compat=%zu\n", compat_nonzero_completed);
        }
        for (i = 0; i < SKIP_REASON_COUNT; i++) {
            if (skip_reason_counts[i] > 0) {
                printf("    skip[%s]=%zu\n", k_skip_reasons[i], skip_reason_counts[i]);
            }
        }
        printf("  Summary: %s\n", summary_path);
        printf("  Failure details: %s\n", fail_jsonl_path);
        printf("  Failure summary: %s\n", fail_summary_path);

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
    free(fail_log_dir);
    free(jsonl_path);
    free(summary_path);
    free(fail_jsonl_path);
    free(fail_summary_path);
    strlist_free(&tests);
    optlist_free(&opts);
    rowlist_free(&rows);
    return exit_code;
}
