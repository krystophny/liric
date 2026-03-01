#include "bench_common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *k_bench_mode_names[BENCH_MODE_COUNT] = {"isel", "copy_patch", "llvm"};
enum {
    BENCH_TRANSIENT_RETRY_ATTEMPTS = 5,
    BENCH_TRANSIENT_RETRY_BASE_NS = 10000000L
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int is_transient_spawn_errno(int err) {
    return err == EAGAIN || err == EMFILE || err == ENFILE;
}

static void transient_retry_backoff(int attempt_idx) {
    struct timespec ts;
    long delay_ns = BENCH_TRANSIENT_RETRY_BASE_NS * (long)(attempt_idx + 1);
    if (delay_ns > 50000000L) delay_ns = 50000000L;
    ts.tv_sec = 0;
    ts.tv_nsec = delay_ns;
    nanosleep(&ts, NULL);
}

static int mkstemp_with_retry(char *tpl, int *err_out) {
    int fd = -1;
    int attempt;
    int err = 0;
    for (attempt = 0; attempt < BENCH_TRANSIENT_RETRY_ATTEMPTS; attempt++) {
        fd = mkstemp(tpl);
        if (fd >= 0) {
            if (err_out) *err_out = 0;
            return fd;
        }
        err = errno;
        if (!is_transient_spawn_errno(err)) break;
        if (attempt + 1 < BENCH_TRANSIENT_RETRY_ATTEMPTS)
            transient_retry_backoff(attempt);
    }
    if (err_out) *err_out = err;
    return -1;
}

static pid_t fork_with_retry(int *err_out) {
    pid_t pid = -1;
    int attempt;
    int err = 0;
    for (attempt = 0; attempt < BENCH_TRANSIENT_RETRY_ATTEMPTS; attempt++) {
        pid = fork();
        if (pid >= 0) {
            if (err_out) *err_out = 0;
            return pid;
        }
        err = errno;
        if (!is_transient_spawn_errno(err)) break;
        if (attempt + 1 < BENCH_TRANSIENT_RETRY_ATTEMPTS)
            transient_retry_backoff(attempt);
    }
    if (err_out) *err_out = err;
    return -1;
}

static void set_spawn_error(bench_cmd_result_t *out, const char *stage, int err) {
    char msg[256];
    if (!out) return;
    out->sys_errno = err;
    if (out->spawn_error_text) {
        free(out->spawn_error_text);
        out->spawn_error_text = NULL;
    }
    if (!stage) stage = "unknown";
    if (err > 0)
        snprintf(msg, sizeof(msg), "%s failed: %s", stage, strerror(err));
    else
        snprintf(msg, sizeof(msg), "%s failed", stage);
    out->spawn_error_text = bench_xstrdup(msg);
}

char *bench_xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

char *bench_to_abs_path(const char *path) {
    char cwd[PATH_MAX];
    size_t nc, np;
    char *out;
    if (!path) return NULL;
    if (path[0] == '/') return bench_xstrdup(path);
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    nc = strlen(cwd);
    np = strlen(path);
    out = (char *)malloc(nc + 1 + np + 1);
    if (!out) return NULL;
    memcpy(out, cwd, nc);
    out[nc] = '/';
    memcpy(out + nc + 1, path, np + 1);
    return out;
}

char *bench_path_join2(const char *a, const char *b) {
    size_t na, nb;
    int need;
    char *out;
    if (!a || !b) return NULL;
    na = strlen(a);
    nb = strlen(b);
    need = (na > 0 && a[na - 1] != '/');
    out = (char *)malloc(na + nb + (need ? 2 : 1));
    if (!out) return NULL;
    memcpy(out, a, na);
    if (need) out[na++] = '/';
    memcpy(out + na, b, nb);
    out[na + nb] = '\0';
    return out;
}

char *bench_dirname_dup(const char *path) {
    const char *slash;
    size_t n;
    char *out;
    if (!path) return NULL;
    slash = strrchr(path, '/');
    if (!slash) return bench_xstrdup(".");
    n = (size_t)(slash - path);
    if (n == 0) n = 1;
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

char *bench_read_all_file(const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    size_t nread;
    char *buf;
    if (!f) return bench_xstrdup("");
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return bench_xstrdup("");
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return bench_xstrdup("");
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return bench_xstrdup("");
    }
    buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return bench_xstrdup("");
    }
    nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

static int wait_with_timeout(pid_t pid, int timeout_ms, int timeout_grace_ms, int *status_out) {
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
        const double deadline_ms = now_ms() + (double)timeout_ms + (double)timeout_grace_ms;
        for (;;) {
            r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                *status_out = status;
                return 0;
            }
            if (r == 0) {
                struct timespec ts;
                if (now_ms() >= deadline_ms) {
                    pid_t pgid;
                    r = waitpid(pid, &status, WNOHANG);
                    if (r == pid) {
                        *status_out = status;
                        return 0;
                    }
                    pgid = getpgid(pid);
                    if (pgid == pid) {
                        (void)kill(-pgid, SIGKILL);
                    }
                    (void)kill(pid, SIGKILL);
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
                ts.tv_nsec = 100000L;
                nanosleep(&ts, NULL);
                continue;
            }
            if (errno == EINTR) continue;
            *status_out = 0;
            return -1;
        }
    }
}

int bench_run_cmd(const bench_run_cmd_opts_t *opts, bench_cmd_result_t *out) {
    char out_tpl[] = "/tmp/liric_cmd_out_XXXXXX";
    char err_tpl[] = "/tmp/liric_cmd_err_XXXXXX";
    const char *out_read_path = out_tpl;
    int out_fd = -1;
    int err_fd = -1;
    int out_tpl_active = 0;
    int err_tpl_active = 0;
    int status = 0;
    pid_t pid;
    double t0;
    int io_errno = 0;
    int spawn_errno = 0;
    int wait_rc = 0;

    if (!opts || !opts->argv || !opts->argv[0] || !out) return -1;

    out->rc = -1;
    out->stdout_text = bench_xstrdup("");
    out->stderr_text = bench_xstrdup("");
    out->elapsed_ms = 0.0;
    out->timed_out = 0;
    out->sys_errno = 0;
    out->spawn_error_text = NULL;
    if (!out->stdout_text || !out->stderr_text) goto fail;

    if (opts->stdout_path && opts->stdout_path[0] != '\0') {
        out_read_path = opts->stdout_path;
    } else {
        out_fd = mkstemp_with_retry(out_tpl, &io_errno);
        if (out_fd < 0) {
            set_spawn_error(out, "mkstemp(stdout)", io_errno);
            goto fail;
        }
        out_tpl_active = 1;
    }
    err_fd = mkstemp_with_retry(err_tpl, &io_errno);
    if (err_fd < 0) {
        set_spawn_error(out, "mkstemp(stderr)", io_errno);
        goto fail;
    }
    err_tpl_active = 1;

    pid = fork_with_retry(&spawn_errno);
    if (pid < 0) {
        set_spawn_error(out, "fork", spawn_errno);
        goto fail;
    }

    if (pid == 0) {
        int fdout = out_fd;
        int fderr = err_fd;
        setpgid(0, 0);
        if (opts->work_dir && chdir(opts->work_dir) != 0) _exit(127);
        if (opts->env_lib_dir) {
            setenv("DYLD_LIBRARY_PATH", opts->env_lib_dir, 1);
            setenv("LD_LIBRARY_PATH", opts->env_lib_dir, 1);
        }
        if (opts->stdout_path && opts->stdout_path[0] != '\0') {
            fdout = open(opts->stdout_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
            if (fdout < 0) _exit(127);
        }
        {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        if (dup2(fdout, STDOUT_FILENO) < 0) _exit(127);
        if (dup2(fderr, STDERR_FILENO) < 0) _exit(127);
        close(fdout);
        close(fderr);
        execvp(opts->argv[0], opts->argv);
        _exit(127);
    }

    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        /* Child may have already exec'd; timeout path re-checks getpgid(). */
    }

    t0 = now_ms();
    if (out_fd >= 0) {
        close(out_fd);
        out_fd = -1;
    }
    if (err_fd >= 0) {
        close(err_fd);
        err_fd = -1;
    }

    {
        int timeout_grace_ms = opts->timeout_grace_ms;
        if (timeout_grace_ms < 0) timeout_grace_ms = 0;
        wait_rc = wait_with_timeout(pid, opts->timeout_ms, timeout_grace_ms, &status);
        if (wait_rc == 1) {
            out->timed_out = 1;
            out->rc = -99;
        } else if (wait_rc < 0) {
            set_spawn_error(out, "waitpid", errno);
            out->rc = -1;
        } else if (WIFEXITED(status)) {
            out->rc = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            out->rc = -WTERMSIG(status);
        } else {
            out->rc = -1;
        }
    }
    out->elapsed_ms = now_ms() - t0;

    free(out->stdout_text);
    free(out->stderr_text);
    out->stdout_text = bench_read_all_file(out_read_path);
    out->stderr_text = bench_read_all_file(err_tpl);
    if (!out->stdout_text) out->stdout_text = bench_xstrdup("");
    if (!out->stderr_text) out->stderr_text = bench_xstrdup("");

    if (!opts->stdout_path || opts->stdout_path[0] == '\0') {
        if (out_tpl_active) unlink(out_tpl);
    }
    if (err_tpl_active) unlink(err_tpl);
    return 0;

fail:
    if (out_fd >= 0) close(out_fd);
    if (err_fd >= 0) close(err_fd);
    if (out_tpl_active) unlink(out_tpl);
    if (err_tpl_active) unlink(err_tpl);
    if (!out->stdout_text) out->stdout_text = bench_xstrdup("");
    if (!out->stderr_text) out->stderr_text = bench_xstrdup("");
    return -1;
}

int bench_run_cmd_with_mode(const char *mode, const bench_run_cmd_opts_t *opts, bench_cmd_result_t *out) {
    const char *old_mode;
    char *old_mode_copy;
    int rc;

    if (!mode || !bench_is_supported_mode(mode) || !opts || !out)
        return -1;

    old_mode = getenv("LIRIC_COMPILE_MODE");
    old_mode_copy = old_mode ? bench_xstrdup(old_mode) : NULL;
    if (setenv("LIRIC_COMPILE_MODE", mode, 1) != 0) {
        free(old_mode_copy);
        return -1;
    }

    rc = bench_run_cmd(opts, out);

    if (old_mode_copy) {
        setenv("LIRIC_COMPILE_MODE", old_mode_copy, 1);
        free(old_mode_copy);
    } else {
        unsetenv("LIRIC_COMPILE_MODE");
    }

    return rc;
}

void bench_free_cmd_result(bench_cmd_result_t *r) {
    if (!r) return;
    free(r->stdout_text);
    free(r->stderr_text);
    free(r->spawn_error_text);
    r->stdout_text = NULL;
    r->stderr_text = NULL;
    r->spawn_error_text = NULL;
    r->sys_errno = 0;
}

int bench_is_supported_mode(const char *mode) {
    size_t i;
    if (!mode) return 0;
    for (i = 0; i < BENCH_MODE_COUNT; i++) {
        if (strcmp(mode, k_bench_mode_names[i]) == 0) return 1;
    }
    return 0;
}

const char *bench_mode_name(size_t mode_idx) {
    if (mode_idx >= BENCH_MODE_COUNT) return NULL;
    return k_bench_mode_names[mode_idx];
}

int bench_parse_modes_csv(const char *csv, int *modes_out, size_t modes_len) {
    char *tmp;
    char *saveptr = NULL;
    char *tok;
    size_t i;
    int any = 0;

    if (!csv || !modes_out || modes_len < BENCH_MODE_COUNT) return -1;

    if (strcmp(csv, "all") == 0) {
        for (i = 0; i < BENCH_MODE_COUNT; i++) modes_out[i] = 1;
        return 0;
    }

    for (i = 0; i < BENCH_MODE_COUNT; i++) modes_out[i] = 0;

    tmp = bench_xstrdup(csv);
    tok = strtok_r(tmp, ",", &saveptr);
    while (tok) {
        int found = 0;
        for (i = 0; i < BENCH_MODE_COUNT; i++) {
            if (strcmp(tok, k_bench_mode_names[i]) == 0) {
                modes_out[i] = 1;
                found = 1;
                any = 1;
                break;
            }
        }
        if (!found) {
            free(tmp);
            return -1;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    free(tmp);

    return any ? 0 : -1;
}

static int cmp_double(const void *a, const void *b) {
    const double *da = (const double *)a;
    const double *db = (const double *)b;
    if (*da < *db) return -1;
    if (*da > *db) return 1;
    return 0;
}

double bench_median(const double *vals, size_t n) {
    double *tmp;
    double out;
    if (n == 0) return 0.0;
    tmp = (double *)malloc(n * sizeof(double));
    if (!tmp) return 0.0;
    memcpy(tmp, vals, n * sizeof(double));
    qsort(tmp, n, sizeof(double), cmp_double);
    if (n % 2 == 0) out = 0.5 * (tmp[n / 2 - 1] + tmp[n / 2]);
    else out = tmp[n / 2];
    free(tmp);
    return out;
}

double bench_percentile(const double *vals, size_t n, double p) {
    double *tmp;
    double k, frac, out;
    size_t f, c;
    if (n == 0) return 0.0;
    if (p < 0.0) p = 0.0;
    if (p > 100.0) p = 100.0;
    tmp = (double *)malloc(n * sizeof(double));
    if (!tmp) return 0.0;
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

int bench_json_get_number(const char *json, const char *key, double *out_val) {
    const char *p;
    if (!json || !key || !out_val) return 0;
    p = strstr(json, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (!*p) return 0;
    *out_val = strtod(p, NULL);
    return 1;
}
