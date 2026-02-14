// Focused corpus benchmark: runs 100 curated tests through liric_probe_runner
// with timing instrumentation. Supports profiling modes.
//
// Usage:
//   ./build/bench_corpus                    # run all 100, print timing table
//   ./build/bench_corpus --top 10           # show top 10 slowest
//   ./build/bench_corpus --csv              # output CSV for analysis
//   ./build/bench_corpus --single <name>    # run one test (for perf/callgrind)

#include <fcntl.h>
#include <errno.h>
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

#define MAX_TESTS 200
#define MAX_LINE 1024

typedef struct {
    char case_id[64];
    char name[128];
    int ll_size;
    char ll_path[PATH_MAX];
} corpus_entry_t;

typedef struct {
    char name[128];
    double read_us;
    double parse_us;
    double compile_us;
    double lookup_us;
    double exec_us;
    double total_us;
    int ok;
} timing_result_t;

typedef struct {
    const char *probe_runner;
    const char *runtime_bc;
    const char *lfortran_src;
    const char *corpus_tsv;
    const char *cache_dir;
    int timeout_sec;
} bench_cfg_t;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n;
    if (!path || !path[0]) return -1;
    n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);

    if (tmp[n - 1] == '/')
        tmp[n - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static char *dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t n;
    char *out;
    if (!slash) {
        out = (char *)malloc(2);
        if (!out) return NULL;
        out[0] = '.';
        out[1] = '\0';
        return out;
    }
    n = (size_t)(slash - path);
    if (n == 0) n = 1;
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

static char *path_join2(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    int need = (na > 0 && a[na - 1] != '/');
    char *out = (char *)malloc(na + nb + (need ? 2 : 1));
    if (!out) return NULL;
    memcpy(out, a, na);
    if (need) out[na++] = '/';
    memcpy(out + na, b, nb);
    out[na + nb] = '\0';
    return out;
}

static int run_exec(char *const argv[]) {
    int status = 0;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static int ensure_runtime_bc(const char *lfortran_src, const char *runtime_bc_path) {
    char *runtime_c = NULL;
    char *include_dir = NULL;
    char *out_dir = NULL;
    int rc = -1;

    if (file_exists(runtime_bc_path))
        return 0;

    runtime_c = path_join2(lfortran_src, "src/libasr/runtime/lfortran_intrinsics.c");
    include_dir = path_join2(lfortran_src, "src");
    if (!runtime_c || !include_dir) {
        fprintf(stderr, "out of memory while preparing runtime bc build\n");
        goto cleanup;
    }
    if (!file_exists(runtime_c)) {
        fprintf(stderr, "lfortran runtime source not found: %s\n", runtime_c);
        goto cleanup;
    }
    out_dir = dirname_dup(runtime_bc_path);
    if (!out_dir || mkdir_p(out_dir) != 0) {
        fprintf(stderr, "failed to create runtime bc directory: %s\n",
                out_dir ? out_dir : "(null)");
        goto cleanup;
    }

    {
        char include_flag[PATH_MAX + 3];
        snprintf(include_flag, sizeof(include_flag), "-I%s", include_dir);
        char *argv[] = {
            "clang",
            "-c",
            "-emit-llvm",
            "-O2",
            "-fno-exceptions",
            "-fno-unwind-tables",
            include_flag,
            "-o",
            (char *)runtime_bc_path,
            runtime_c,
            NULL
        };
        fprintf(stderr, "Generating runtime bc: %s\n", runtime_bc_path);
        rc = run_exec(argv);
        if (rc != 0) {
            fprintf(stderr, "failed to compile runtime bc (clang rc=%d)\n", rc);
            goto cleanup;
        }
    }

    rc = 0;
cleanup:
    free(runtime_c);
    free(include_dir);
    free(out_dir);
    return rc;
}

static int load_corpus(const char *tsv_path, const char *cache_dir,
                       corpus_entry_t *entries, int max_entries) {
    FILE *f = fopen(tsv_path, "r");
    if (!f) {
        fprintf(stderr, "cannot open corpus: %s\n", tsv_path);
        return -1;
    }
    int n = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && n < max_entries) {
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab2 = '\0';

        corpus_entry_t *e = &entries[n];
        snprintf(e->case_id, sizeof(e->case_id), "%s", line);
        snprintf(e->name, sizeof(e->name), "%s", tab1 + 1);

        char *endp = NULL;
        e->ll_size = (int)strtol(tab2 + 1, &endp, 10);

        snprintf(e->ll_path, sizeof(e->ll_path), "%s/%s/raw.ll",
                 cache_dir, e->case_id);

        struct stat st;
        if (stat(e->ll_path, &st) == 0) {
            n++;
        }
    }
    fclose(f);
    return n;
}

static int run_timed(const char *probe_runner, const char *runtime_bc,
                     const char *ll_path, timing_result_t *result,
                     int timeout_sec) {
    int stderr_pipe[2];
    if (pipe(stderr_pipe)) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        close(stderr_pipe[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[1]);

        const char *args[] = {
            probe_runner, "--timing", "--ignore-retcode",
            "--runtime-bc", runtime_bc,
            "--func", "main", "--sig", "i32_argc_argv",
            ll_path, NULL
        };
        execvp(args[0], (char *const *)args);
        _exit(127);
    }

    close(stderr_pipe[1]);

    // Read stderr (contains TIMING line) — stdout goes to /dev/null
    char stderr_buf[4096];
    memset(stderr_buf, 0, sizeof(stderr_buf));
    size_t stderr_len = 0;
    while (stderr_len < sizeof(stderr_buf) - 1) {
        ssize_t n = read(stderr_pipe[0], stderr_buf + stderr_len,
                         sizeof(stderr_buf) - 1 - stderr_len);
        if (n <= 0) break;
        stderr_len += (size_t)n;
    }
    stderr_buf[stderr_len] = '\0';
    close(stderr_pipe[0]);

    // Wait with timeout
    int status = 0;
    double start = now_ms();
    while (1) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w > 0) break;
        if (now_ms() - start > timeout_sec * 1000.0) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            result->ok = 0;
            return -1;
        }
        usleep(1000);
    }

    // Parse TIMING line
    char *timing_line = strstr(stderr_buf, "TIMING ");
    if (!timing_line) {
        result->ok = 0;
        return -1;
    }

    result->ok = 1;
    sscanf(timing_line, "TIMING read_us=%lf parse_us=%lf jit_create_us=%*f "
           "load_lib_us=%*f compile_us=%lf lookup_us=%lf exec_us=%lf total_us=%lf",
           &result->read_us, &result->parse_us,
           &result->compile_us, &result->lookup_us,
           &result->exec_us, &result->total_us);
    return 0;
}

static int cmp_by_compile(const void *a, const void *b) {
    const timing_result_t *ta = a, *tb = b;
    double da = ta->parse_us + ta->compile_us;
    double db = tb->parse_us + tb->compile_us;
    if (da < db) return 1;
    if (da > db) return -1;
    return 0;
}

static void set_default_cfg(bench_cfg_t *cfg) {
    cfg->probe_runner = "./build/liric_probe_runner";
    cfg->runtime_bc = "/tmp/liric_bench/runtime/lfortran_intrinsics.bc";
    cfg->lfortran_src = "../lfortran";
    cfg->corpus_tsv = "tools/corpus_100.tsv";
    cfg->cache_dir = "/tmp/liric_lfortran_mass/cache";
    cfg->timeout_sec = 30;
}

static void print_empty_dataset_help(const bench_cfg_t *cfg) {
    fprintf(stderr, "EMPTY DATASET: no tests found in corpus\n");
    fprintf(stderr, "  corpus: %s\n", cfg->corpus_tsv);
    fprintf(stderr, "  cache-dir: %s\n", cfg->cache_dir);
    fprintf(stderr, "  expected: <cache-dir>/<case_id>/raw.ll for entries in corpus TSV\n");
    fprintf(stderr, "  bootstrap cache (default path):\n");
    fprintf(stderr, "    ./tools/lfortran_mass/nightly_mass.sh --output-root /tmp/liric_lfortran_mass\n");
    fprintf(stderr, "  override cache location with: --cache-dir PATH\n");
}

int main(int argc, char **argv) {
    bench_cfg_t cfg;
    int top_n = 0;
    int csv_mode = 0;
    int allow_empty = 0;
    const char *single_name = NULL;
    int iters = 1;

    set_default_cfg(&cfg);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top_n = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--csv") == 0) {
            csv_mode = 1;
        } else if (strcmp(argv[i], "--single") == 0 && i + 1 < argc) {
            single_name = argv[++i];
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg.probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--runtime-bc") == 0 && i + 1 < argc) {
            cfg.runtime_bc = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-src") == 0 && i + 1 < argc) {
            cfg.lfortran_src = argv[++i];
        } else if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            cfg.corpus_tsv = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cfg.cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0)
                cfg.timeout_sec = 30;
        } else if (strcmp(argv[i], "--allow-empty") == 0) {
            allow_empty = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: bench_corpus [--top N] [--csv] [--single NAME] [--iters N]\n");
            printf("                   [--probe-runner PATH] [--runtime-bc PATH]\n");
            printf("                   [--lfortran-src PATH] [--corpus PATH] [--cache-dir PATH]\n");
            printf("                   [--timeout SEC] [--allow-empty]\n");
            return 0;
        }
    }

    if (!file_exists(cfg.probe_runner)) {
        fprintf(stderr, "probe runner not found: %s\n", cfg.probe_runner);
        return 1;
    }
    if (!file_exists(cfg.runtime_bc)) {
        if (ensure_runtime_bc(cfg.lfortran_src, cfg.runtime_bc) != 0)
            return 1;
    }
    if (!file_exists(cfg.runtime_bc)) {
        fprintf(stderr, "runtime bc not found: %s\n", cfg.runtime_bc);
        return 1;
    }

    corpus_entry_t entries[MAX_TESTS];
    int n = load_corpus(cfg.corpus_tsv, cfg.cache_dir, entries, MAX_TESTS);
    if (n <= 0) {
        print_empty_dataset_help(&cfg);
        if (csv_mode)
            printf("name,parse_us,compile_us,jit_us,exec_us,total_us\n");
        printf("Status: EMPTY DATASET\n");
        return allow_empty ? 0 : 1;
    }

    // Filter to single test if requested
    if (single_name) {
        for (int i = 0; i < n; i++) {
            if (strcmp(entries[i].name, single_name) == 0) {
                entries[0] = entries[i];
                n = 1;
                goto found;
            }
        }
        fprintf(stderr, "test '%s' not found in corpus\n", single_name);
        return 1;
    }
found:

    fprintf(stderr, "Corpus: %d tests, runtime-bc: %s\n", n, cfg.runtime_bc);
    fprintf(stderr, "Iterations: %d\n\n", iters);

    timing_result_t results[MAX_TESTS];
    memset(results, 0, sizeof(results));
    int ok_count = 0;

    for (int iter = 0; iter < iters; iter++) {
        if (iters > 1) fprintf(stderr, "--- Iteration %d/%d ---\n", iter + 1, iters);
        for (int i = 0; i < n; i++) {
            timing_result_t r = {0};
            snprintf(r.name, sizeof(r.name), "%s", entries[i].name);

            if (!csv_mode && !single_name) {
                fprintf(stderr, "\r[%d/%d] %s", i + 1, n, entries[i].name);
                fflush(stderr);
            }

            run_timed(cfg.probe_runner, cfg.runtime_bc, entries[i].ll_path, &r,
                      cfg.timeout_sec);

            if (r.ok) {
                if (iter == 0) {
                    results[i] = r;
                    ok_count++;
                } else {
                    // Keep minimum (best) times
                    double cur = results[i].parse_us + results[i].compile_us;
                    double new_val = r.parse_us + r.compile_us;
                    if (new_val < cur) {
                        double old_exec = results[i].exec_us;
                        results[i] = r;
                        results[i].exec_us = old_exec; // keep first exec time
                    }
                }
            } else if (iter == 0) {
                snprintf(results[i].name, sizeof(results[i].name),
                         "%s", entries[i].name);
            }
        }
        if (!csv_mode) fprintf(stderr, "\r%*s\r", 60, "");
    }

    if (ok_count == 0) {
        fprintf(stderr, "EMPTY DATASET: no runnable tests completed (attempted=%d)\n", n);
        if (csv_mode)
            printf("name,parse_us,compile_us,jit_us,exec_us,total_us\n");
        printf("Status: EMPTY DATASET\n");
        return allow_empty ? 0 : 1;
    }

    // Sort by compile time (parse + compile)
    qsort(results, (size_t)n, sizeof(timing_result_t), cmp_by_compile);

    if (csv_mode) {
        printf("name,parse_us,compile_us,jit_us,exec_us,total_us\n");
        for (int i = 0; i < n; i++) {
            if (!results[i].ok) continue;
            printf("%s,%.1f,%.1f,%.1f,%.1f,%.1f\n",
                   results[i].name,
                   results[i].parse_us, results[i].compile_us,
                   results[i].parse_us + results[i].compile_us,
                   results[i].exec_us, results[i].total_us);
        }
        return 0;
    }

    // Print table
    int show = (top_n > 0 && top_n < ok_count) ? top_n : ok_count;

    printf("%-45s %10s %10s %10s %10s\n",
           "Test", "Parse(us)", "Compile(us)", "JIT(us)", "Exec(us)");
    printf("%-45s %10s %10s %10s %10s\n",
           "----", "---------", "-----------", "-------", "--------");

    double sum_parse = 0, sum_compile = 0, sum_exec = 0;
    for (int i = 0; i < n; i++) {
        if (!results[i].ok) continue;
        sum_parse += results[i].parse_us;
        sum_compile += results[i].compile_us;
        sum_exec += results[i].exec_us;

        if (i < show) {
            printf("%-45s %10.0f %11.0f %10.0f %10.0f\n",
                   results[i].name,
                   results[i].parse_us, results[i].compile_us,
                   results[i].parse_us + results[i].compile_us,
                   results[i].exec_us);
        }
    }

    printf("\n%-45s %10.0f %11.0f %10.0f %10.0f\n",
           "TOTAL", sum_parse, sum_compile,
           sum_parse + sum_compile, sum_exec);
    printf("%-45s %10.1f %11.1f %10.1f %10.1f ms\n",
           "", sum_parse / 1e3, sum_compile / 1e3,
           (sum_parse + sum_compile) / 1e3, sum_exec / 1e3);

    printf("\nPassed: %d/%d\n", ok_count, n);
    printf("Parse:   %6.1f ms (%.0f%%)\n", sum_parse / 1e3,
           100.0 * sum_parse / (sum_parse + sum_compile));
    printf("Compile: %6.1f ms (%.0f%%)\n", sum_compile / 1e3,
           100.0 * sum_compile / (sum_parse + sum_compile));
    printf("JIT total: %5.1f ms\n", (sum_parse + sum_compile) / 1e3);
    printf("Status: OK\n");

    return 0;
}
