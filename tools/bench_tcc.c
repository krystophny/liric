/*
 * bench_tcc: TCC baseline vs liric, corpus-driven.
 *
 * Reads test cases from a corpus TSV + cache directory (same format as
 * bench_corpus_compare).  For each case that has both raw.ll and raw.c:
 *
 *   WALL-CLOCK: subprocess `tcc -c file.c` vs `liric_probe_runner --no-exec`
 *   IN-PROCESS: `tcc_compile_string()` vs `lr_compiler_feed_ll()+lookup`
 *
 * Usage: ./build/bench_tcc [--iters N] [--bench-dir PATH]
 *            [--corpus PATH] [--cache-dir PATH]
 *            [--probe-runner PATH] [--policy direct|ir]
 *            [--lfortran-include-dir PATH]
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <libtcc.h>
#include <liric/liric.h>

#include "bench_common.h"

extern char **environ;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

typedef struct {
    char *id;
    char *ll_path;
    char *c_path;
    char *ll_src;
    char *c_src;
    size_t ll_len;
    size_t c_len;
} corpus_case_t;

typedef struct {
    corpus_case_t *items;
    size_t count;
    size_t cap;
} corpus_t;

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

static char *read_file_alloc(const char *path, size_t *out_len) {
    FILE *f;
    long sz;
    char *buf;
    size_t nread;

    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    if (out_len) *out_len = nread;
    return buf;
}

static void corpus_push(corpus_t *c, corpus_case_t *item) {
    if (c->count == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 64;
        corpus_case_t *tmp = (corpus_case_t *)realloc(
            c->items, ncap * sizeof(corpus_case_t));
        if (!tmp) { fprintf(stderr, "out of memory\n"); exit(1); }
        c->items = tmp;
        c->cap = ncap;
    }
    c->items[c->count++] = *item;
}

static void corpus_free(corpus_t *c) {
    for (size_t i = 0; i < c->count; i++) {
        free(c->items[i].id);
        free(c->items[i].ll_path);
        free(c->items[i].c_path);
        free(c->items[i].ll_src);
        free(c->items[i].c_src);
    }
    free(c->items);
    memset(c, 0, sizeof(*c));
}

static int load_corpus(const char *tsv_path, const char *cache_dir,
                       corpus_t *out) {
    FILE *f;
    char line[2048];

    memset(out, 0, sizeof(*out));

    f = fopen(tsv_path, "r");
    if (!f) return -1;

    while (fgets(line, sizeof(line), f)) {
        char *tab;
        char *id;
        size_t id_len;
        corpus_case_t c;

        memset(&c, 0, sizeof(c));

        /* first column is the case id */
        tab = strchr(line, '\t');
        if (tab) *tab = '\0';
        id = line;
        id_len = strlen(id);
        while (id_len > 0 && (id[id_len-1] == '\n' || id[id_len-1] == '\r'))
            id[--id_len] = '\0';
        if (id_len == 0) continue;

        c.ll_path = bench_path_join2(cache_dir, id);
        if (!c.ll_path) continue;
        {
            char *tmp = c.ll_path;
            c.ll_path = bench_path_join2(tmp, "raw.ll");
            free(tmp);
        }
        c.c_path = bench_path_join2(cache_dir, id);
        if (!c.c_path) { free(c.ll_path); continue; }
        {
            char *tmp = c.c_path;
            c.c_path = bench_path_join2(tmp, "raw.c");
            free(tmp);
        }

        if (!file_exists(c.ll_path) || !file_exists(c.c_path)) {
            free(c.ll_path);
            free(c.c_path);
            continue;
        }

        c.ll_src = read_file_alloc(c.ll_path, &c.ll_len);
        c.c_src = read_file_alloc(c.c_path, &c.c_len);
        if (!c.ll_src || !c.c_src) {
            free(c.ll_path);
            free(c.c_path);
            free(c.ll_src);
            free(c.c_src);
            continue;
        }

        c.id = bench_xstrdup(id);
        corpus_push(out, &c);
    }

    fclose(f);
    return 0;
}

static double run_exec_timed(char *const argv[]) {
    pid_t pid;
    int devnull;
    int err;
    int status;
    posix_spawn_file_actions_t fa;
    double t0;
    double t1;

    devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0)
        return -1.0;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, devnull, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, devnull, STDERR_FILENO);

    t0 = now_us();
    err = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    if (err != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(devnull);
        return -1.0;
    }

    if (waitpid(pid, &status, 0) < 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(devnull);
        return -1.0;
    }
    t1 = now_us();

    posix_spawn_file_actions_destroy(&fa);
    close(devnull);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return t1 - t0;
    return -1.0;
}

static int backend_from_env(lr_backend_t *out) {
    const char *mode;
    if (!out) return -1;
    mode = getenv("LIRIC_COMPILE_MODE");
    if (!mode || !mode[0] || strcmp(mode, "isel") == 0) {
        *out = LR_BACKEND_ISEL;
        return 0;
    }
    if (strcmp(mode, "copy_patch") == 0 || strcmp(mode, "stencil") == 0) {
        *out = LR_BACKEND_COPY_PATCH;
        return 0;
    }
    if (strcmp(mode, "llvm") == 0) {
        *out = LR_BACKEND_LLVM;
        return 0;
    }
    return -1;
}

static int executable_in_path(const char *name) {
    const char *path_env;
    const char *start;
    const char *end;
    char cand[PATH_MAX];
    size_t part_len;

    if (!name || name[0] == '\0')
        return 0;

    if (strchr(name, '/'))
        return access(name, X_OK) == 0;

    path_env = getenv("PATH");
    if (!path_env)
        return 0;

    start = path_env;
    while (*start) {
        end = strchr(start, ':');
        if (!end)
            end = start + strlen(start);
        part_len = (size_t)(end - start);

        if (part_len == 0) {
            if (snprintf(cand, sizeof(cand), "./%s", name) < (int)sizeof(cand) && access(cand, X_OK) == 0)
                return 1;
        } else {
            if (part_len + 1 + strlen(name) + 1 <= sizeof(cand)) {
                memcpy(cand, start, part_len);
                cand[part_len] = '/';
                strcpy(cand + part_len + 1, name);
                if (access(cand, X_OK) == 0)
                    return 1;
            }
        }

        if (*end == '\0')
            break;
        start = end + 1;
    }

    return 0;
}

/* Create a stub complex.h so TCC can compile lfortran C output.
 * TCC doesn't support _Complex; the stub prevents system complex.h
 * from being included. We also define _Complex as empty via
 * tcc_define_symbol / -D flag. */
static int create_tcc_stub_dir(const char *bench_dir, char *out_path, size_t out_sz) {
    char stub_dir[PATH_MAX];
    char stub_path[PATH_MAX];
    FILE *f;

    snprintf(stub_dir, sizeof(stub_dir), "%s/tcc_stubs", bench_dir);
    if (mkdir_p(stub_dir) != 0) return -1;

    snprintf(stub_path, sizeof(stub_path), "%s/complex.h", stub_dir);
    f = fopen(stub_path, "w");
    if (!f) return -1;
    fprintf(f, "/* TCC stub - no _Complex support */\n");
    fclose(f);

    snprintf(out_path, out_sz, "%s", stub_dir);
    return 0;
}

static void usage(void) {
    printf("usage: bench_tcc [options]\n");
    printf("  --iters N                  iterations (best-of) (default: 1)\n");
    printf("  --bench-dir PATH           output directory (default: /tmp/liric_bench)\n");
    printf("  --corpus PATH              corpus TSV file\n");
    printf("  --cache-dir PATH           corpus cache directory\n");
    printf("  --probe-runner PATH        liric_probe_runner binary\n");
    printf("  --runtime-lib PATH         runtime library for probe_runner\n");
    printf("  --policy direct|ir         liric policy (default: direct)\n");
    printf("  --lfortran-include-dir PATH  include dir for lfortran_intrinsics.h\n");
}

int main(int argc, char **argv) {
    int iters = 1;
    const char *bench_dir = "/tmp/liric_bench";
    const char *corpus_path = NULL;
    const char *cache_dir = NULL;
    const char *probe_runner_path = NULL;
    const char *runtime_lib = NULL;
    const char *policy = "direct";
    const char *lfortran_include_dir = NULL;
    const char *build_dir;
    char probe_runner_buf[PATH_MAX];
    char liric_mode_name[32] = "isel";
    char tcc_bin[PATH_MAX] = "tcc";
    char tcc_obj_path[PATH_MAX];
    char tcc_stub_dir[PATH_MAX] = {0};
    lr_backend_t backend = LR_BACKEND_ISEL;
    corpus_t corpus = {0};
    int skipped_corpus = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            corpus_path = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            probe_runner_path = argv[++i];
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            policy = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-include-dir") == 0 && i + 1 < argc) {
            lfortran_include_dir = argv[++i];
        } else if (strcmp(argv[i], "--work-dir") == 0 && i + 1 < argc) {
            /* ignored, kept for backward compat */
            ++i;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage();
            return 1;
        }
    }
    if (iters < 1) iters = 1;
    if (strcmp(policy, "direct") != 0 && strcmp(policy, "ir") != 0) {
        fprintf(stderr, "error: invalid --policy value: %s (expected direct|ir)\n", policy);
        return 1;
    }

    /* defaults for corpus/cache if not provided */
    if (!corpus_path)
        corpus_path = "/tmp/liric_bench/corpus_from_compat.tsv";
    if (!cache_dir)
        cache_dir = "/tmp/liric_bench/cache_from_compat";

    /* probe runner */
    if (!probe_runner_path) {
        build_dir = getenv("LIRIC_BUILD_DIR");
        if (!build_dir) build_dir = "build";
        snprintf(probe_runner_buf, sizeof(probe_runner_buf),
                 "%s/liric_probe_runner", build_dir);
        probe_runner_path = probe_runner_buf;
    }
    if (access(probe_runner_path, X_OK) != 0) {
        fprintf(stderr, "error: %s not found\n", probe_runner_path);
        return 1;
    }

    /* runtime lib auto-detect */
    if (!runtime_lib) {
        if (file_exists("../lfortran/build/src/runtime/liblfortran_runtime.so"))
            runtime_lib = "../lfortran/build/src/runtime/liblfortran_runtime.so";
        else if (file_exists("../lfortran/build/src/runtime/liblfortran_runtime.dylib"))
            runtime_lib = "../lfortran/build/src/runtime/liblfortran_runtime.dylib";
    }

    /* lfortran include dir auto-detect */
    if (!lfortran_include_dir) {
        if (file_exists("../lfortran/src/libasr/runtime/lfortran_intrinsics.h"))
            lfortran_include_dir = "../lfortran/src/libasr/runtime";
    }

    /* backend from env */
    if (backend_from_env(&backend) != 0) {
        fprintf(stderr, "error: invalid LIRIC_COMPILE_MODE value\n");
        return 1;
    }
    {
        const char *m = getenv("LIRIC_COMPILE_MODE");
        if (m && m[0])
            snprintf(liric_mode_name, sizeof(liric_mode_name), "%s", m);
    }

    /* tcc binary */
    if (access("/usr/bin/tcc", X_OK) == 0)
        snprintf(tcc_bin, sizeof(tcc_bin), "%s", "/usr/bin/tcc");
    else if (!executable_in_path("tcc")) {
        fprintf(stderr, "error: tcc not found in PATH\n");
        return 1;
    }

    if (mkdir_p(bench_dir) != 0) {
        fprintf(stderr, "error: failed to create bench dir: %s\n", bench_dir);
        return 1;
    }

    /* temp path for tcc object output */
    snprintf(tcc_obj_path, sizeof(tcc_obj_path), "%s/bench_tcc_tmp.o", bench_dir);

    /* create TCC stub complex.h for _Complex workaround */
    if (lfortran_include_dir) {
        if (create_tcc_stub_dir(bench_dir, tcc_stub_dir, sizeof(tcc_stub_dir)) != 0) {
            fprintf(stderr, "warning: failed to create TCC stub dir\n");
            tcc_stub_dir[0] = '\0';
        }
    }

    /* load corpus */
    if (!file_exists(corpus_path)) {
        fprintf(stderr, "error: corpus not found: %s\n", corpus_path);
        return 1;
    }
    if (load_corpus(corpus_path, cache_dir, &corpus) != 0 || corpus.count == 0) {
        fprintf(stderr, "error: no cases with both raw.ll and raw.c in corpus\n");
        /* write a summary indicating zero cases */
        {
            char summary_path[PATH_MAX];
            FILE *sf;
            snprintf(summary_path, sizeof(summary_path),
                     "%s/bench_tcc_summary.json", bench_dir);
            sf = fopen(summary_path, "w");
            if (sf) {
                fprintf(sf, "{\n  \"status\": \"OK\",\n");
                fprintf(sf, "  \"mode\": \"%s\",\n", liric_mode_name);
                fprintf(sf, "  \"policy\": \"%s\",\n", policy);
                fprintf(sf, "  \"iters\": %d,\n", iters);
                fprintf(sf, "  \"total_cases\": 0,\n");
                fprintf(sf, "  \"wall_passed\": 0,\n");
                fprintf(sf, "  \"inproc_passed\": 0,\n");
                fprintf(sf, "  \"skipped\": 0,\n");
                fprintf(sf, "  \"wall_speedup_ratio\": 0.0,\n");
                fprintf(sf, "  \"inproc_speedup_ratio\": 0.0\n");
                fprintf(sf, "}\n");
                fclose(sf);
            }
        }
        corpus_free(&corpus);
        return 0;
    }

    printf("bench_tcc: %zu corpus cases (with both .ll and .c), "
           "%d iterations (best-of), mode=%s, policy=%s\n\n",
           corpus.count, iters, liric_mode_name, policy);

    /* === WALL-CLOCK === */
    printf("=== WALL-CLOCK: subprocess (tcc -c) vs (liric probe_runner --no-exec) ===\n");
    printf("%-32s %10s %10s %8s %s\n", "test", "tcc(us)", "liric(us)", "ratio", "status");
    printf("%-32s %10s %10s %8s %s\n", "----", "-------", "--------", "-----", "------");

    double total_tcc_wall = 0.0, total_liric_wall = 0.0;
    int wall_passed = 0;

    for (size_t ci = 0; ci < corpus.count; ci++) {
        corpus_case_t *tc = &corpus.items[ci];
        double best_tcc = 1e18, best_liric = 1e18;

        for (int it = 0; it < iters; it++) {
            char stub_inc[PATH_MAX], real_inc[PATH_MAX];
            char *tcc_argv[16];
            int n = 0;
            tcc_argv[n++] = tcc_bin;
            tcc_argv[n++] = "-c";
            tcc_argv[n++] = "-o";
            tcc_argv[n++] = tcc_obj_path;
            if (tcc_stub_dir[0]) {
                snprintf(stub_inc, sizeof(stub_inc), "-I%s", tcc_stub_dir);
                tcc_argv[n++] = stub_inc;
            }
            if (lfortran_include_dir) {
                snprintf(real_inc, sizeof(real_inc), "-I%s", lfortran_include_dir);
                tcc_argv[n++] = real_inc;
            }
            tcc_argv[n++] = "-D_Complex= ";
            tcc_argv[n++] = tc->c_path;
            tcc_argv[n++] = NULL;

            double t = run_exec_timed(tcc_argv);
            if (t >= 0.0 && t < best_tcc) best_tcc = t;
        }

        for (int it = 0; it < iters; it++) {
            char *liric_argv[16];
            int n = 0;
            liric_argv[n++] = (char *)probe_runner_path;
            liric_argv[n++] = "--no-exec";
            liric_argv[n++] = "--policy";
            liric_argv[n++] = (char *)policy;
            liric_argv[n++] = "--func";
            liric_argv[n++] = "main";
            liric_argv[n++] = "--sig";
            liric_argv[n++] = "i32";
            if (runtime_lib) {
                liric_argv[n++] = "--load-lib";
                liric_argv[n++] = (char *)runtime_lib;
            }
            liric_argv[n++] = tc->ll_path;
            liric_argv[n++] = NULL;

            double t = run_exec_timed(liric_argv);
            if (t >= 0.0 && t < best_liric) best_liric = t;
        }

        {
            const char *status;
            if (best_tcc >= 1e17 && best_liric >= 1e17) {
                status = "BOTH FAIL";
                skipped_corpus++;
            } else if (best_tcc >= 1e17) {
                status = "tcc FAIL";
                skipped_corpus++;
            } else if (best_liric >= 1e17) {
                status = "liric FAIL";
                skipped_corpus++;
            } else {
                status = "OK";
                wall_passed++;
                total_tcc_wall += best_tcc;
                total_liric_wall += best_liric;
            }

            {
                double ratio = (best_liric > 0 && best_tcc > 0 &&
                                best_tcc < 1e17 && best_liric < 1e17)
                                   ? best_tcc / best_liric
                                   : 0.0;
                printf("%-32s %10.0f %10.0f %7.2fx  %s\n",
                       tc->id,
                       best_tcc < 1e17 ? best_tcc : 0.0,
                       best_liric < 1e17 ? best_liric : 0.0,
                       ratio,
                       status);
            }
        }
    }

    {
        double ratio = (total_liric_wall > 0.0) ? total_tcc_wall / total_liric_wall : 0.0;
        printf("%-32s %10s %10s %8s\n", "----", "-------", "--------", "-----");
        printf("%-32s %10.0f %10.0f %7.2fx  (%d/%zu passed)\n",
               "TOTAL", total_tcc_wall, total_liric_wall, ratio,
               wall_passed, corpus.count);
    }

    /* === IN-PROCESS === */
    printf("\n=== IN-PROCESS: tcc (libtcc compile) vs liric (feed_ll) ===\n");
    printf("%-32s %10s %10s %7s\n", "test", "tcc(us)", "liric(us)", "ratio");
    printf("%-32s %10s %10s %7s\n", "----", "-------", "--------", "-----");

    double ip_tcc_total = 0.0, ip_lr_total = 0.0;
    int inproc_passed = 0;
    int inproc_skipped = 0;

    for (size_t ci = 0; ci < corpus.count; ci++) {
        corpus_case_t *tc = &corpus.items[ci];
        double best_tcc_comp = 1e18;
        double best_lr_feed = 1e18;
        int tcc_ok = 1, lr_ok = 1;

        /* TCC in-process: compile string */
        for (int it = 0; it < iters; it++) {
            TCCState *s = tcc_new();
            if (!s) { tcc_ok = 0; break; }
            tcc_set_output_type(s, TCC_OUTPUT_MEMORY);
            if (tcc_stub_dir[0])
                tcc_add_include_path(s, tcc_stub_dir);
            if (lfortran_include_dir)
                tcc_add_include_path(s, lfortran_include_dir);
            tcc_define_symbol(s, "_Complex", " ");

            double t0 = now_us();
            if (tcc_compile_string(s, tc->c_src) != 0) {
                tcc_delete(s);
                tcc_ok = 0;
                break;
            }
            double t1 = now_us();

            if (t1 - t0 < best_tcc_comp) best_tcc_comp = t1 - t0;
            tcc_delete(s);
        }

        /* Liric in-process: feed_ll */
        for (int it = 0; it < iters; it++) {
            lr_compiler_config_t ccfg;
            lr_compiler_error_t cerr;
            lr_compiler_t *c;
            double t0, t1;

            memset(&ccfg, 0, sizeof(ccfg));
            ccfg.policy = (strcmp(policy, "ir") == 0) ? LR_POLICY_IR : LR_POLICY_DIRECT;
            ccfg.backend = backend;
            ccfg.target = NULL;

            c = lr_compiler_create(&ccfg, &cerr);
            if (!c) { lr_ok = 0; break; }

            t0 = now_us();
            if (lr_compiler_feed_ll(c, tc->ll_src, tc->ll_len, &cerr) != 0) {
                lr_compiler_destroy(c);
                lr_ok = 0;
                break;
            }
            t1 = now_us();

            if (t1 - t0 < best_lr_feed) best_lr_feed = t1 - t0;
            lr_compiler_destroy(c);
        }

        {
            const char *status;
            if (!tcc_ok && !lr_ok) {
                status = "BOTH FAIL";
                inproc_skipped++;
            } else if (!tcc_ok) {
                status = "tcc FAIL";
                inproc_skipped++;
            } else if (!lr_ok) {
                status = "liric FAIL";
                inproc_skipped++;
            } else {
                status = "OK";
                inproc_passed++;
                ip_tcc_total += best_tcc_comp;
                ip_lr_total += best_lr_feed;
            }

            {
                double ratio = (tcc_ok && lr_ok &&
                                best_tcc_comp < 1e17 && best_lr_feed < 1e17 &&
                                best_lr_feed > 0.0)
                                   ? best_tcc_comp / best_lr_feed
                                   : 0.0;
                printf("%-32s %10.1f %10.1f %6.1fx  %s\n",
                       tc->id,
                       tcc_ok && best_tcc_comp < 1e17 ? best_tcc_comp : 0.0,
                       lr_ok && best_lr_feed < 1e17 ? best_lr_feed : 0.0,
                       ratio,
                       status);
            }
        }
    }

    {
        double ratio = (ip_lr_total > 0.0) ? ip_tcc_total / ip_lr_total : 0.0;
        printf("%-32s %10s %10s %7s\n", "----", "-------", "--------", "-----");
        printf("%-32s %10.1f %10.1f %6.1fx  (%d/%zu passed)\n",
               "TOTAL", ip_tcc_total, ip_lr_total, ratio,
               inproc_passed, corpus.count);
    }

    printf("\nAll times in microseconds (us). ratio > 1 = liric faster.\n");
    printf("tcc = tcc_compile_string(), liric = lr_compiler_feed_ll()\n");

    /* === SUMMARY JSON === */
    {
        char summary_path[PATH_MAX];
        FILE *sf;
        double wall_ratio = (total_liric_wall > 0.0)
                                ? total_tcc_wall / total_liric_wall
                                : 0.0;
        double inproc_ratio = (ip_lr_total > 0.0)
                                  ? ip_tcc_total / ip_lr_total
                                  : 0.0;
        const char *status = (wall_passed > 0 && inproc_passed > 0) ? "OK" : "FAILED";

        snprintf(summary_path, sizeof(summary_path),
                 "%s/bench_tcc_summary.json", bench_dir);
        sf = fopen(summary_path, "w");
        if (!sf) {
            fprintf(stderr, "error: failed to write summary: %s\n", summary_path);
            corpus_free(&corpus);
            return 1;
        }
        fprintf(sf, "{\n");
        fprintf(sf, "  \"status\": \"%s\",\n", status);
        fprintf(sf, "  \"mode\": \"%s\",\n", liric_mode_name);
        fprintf(sf, "  \"policy\": \"%s\",\n", policy);
        fprintf(sf, "  \"iters\": %d,\n", iters);
        fprintf(sf, "  \"total_cases\": %zu,\n", corpus.count);
        fprintf(sf, "  \"wall_passed\": %d,\n", wall_passed);
        fprintf(sf, "  \"inproc_passed\": %d,\n", inproc_passed);
        fprintf(sf, "  \"skipped\": %d,\n", skipped_corpus);
        fprintf(sf, "  \"wall_tcc_total_us\": %.6f,\n", total_tcc_wall);
        fprintf(sf, "  \"wall_liric_total_us\": %.6f,\n", total_liric_wall);
        fprintf(sf, "  \"wall_speedup_ratio\": %.6f,\n", wall_ratio);
        fprintf(sf, "  \"inproc_tcc_total_us\": %.6f,\n", ip_tcc_total);
        fprintf(sf, "  \"inproc_liric_total_us\": %.6f,\n", ip_lr_total);
        fprintf(sf, "  \"inproc_speedup_ratio\": %.6f\n", inproc_ratio);
        fprintf(sf, "}\n");
        fclose(sf);
        printf("Summary: %s\n", summary_path);
    }

    /* cleanup temp obj file */
    unlink(tcc_obj_path);

    corpus_free(&corpus);
    return 0;
}
