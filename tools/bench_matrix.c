// Unified benchmark matrix runner.
//
// Canonical behavior:
// - Matrix cells are lane x mode x policy.
// - Primitive lanes are timed directly (api_exe, api_jit, ll_jit, ll_llvm, micro_c).
// - Derived lanes are compatibility aliases/views (api_e2e, ll_e2e, ir_file).
// - api_e2e is retained as a backward-compatible lane id.
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "bench_common.h"

typedef enum {
    MODE_ISEL = 0,
    MODE_COPY_PATCH = 1,
    MODE_LLVM = 2,
    MODE_COUNT = 3
} mode_id_t;

typedef enum {
    LANE_API_EXE = 0,
    LANE_API_JIT = 1,
    LANE_API_E2E = 2,
    LANE_LL_JIT = 3,
    LANE_LL_LLVM = 4,
    LANE_LL_E2E = 5,
    LANE_IR_FILE = 6,
    LANE_MICRO_C = 7,
    LANE_COUNT = 8
} lane_id_t;

typedef enum {
    POLICY_DIRECT = 0,
    POLICY_IR = 1,
    POLICY_COUNT = 2
} policy_id_t;

typedef struct {
    const char *bench_dir;
    const char *build_dir;
    const char *manifest;

    const char *bench_compat_check;
    const char *bench_corpus_compare;
    const char *bench_api;
    const char *bench_tcc;
    const char *probe_runner;
    const char *lli_phases;

    const char *lfortran;
    const char *lfortran_liric;
    const char *lfortran_build_dir;
    const char *lfortran_liric_build_dir;
    const char *cmake;
    const char *test_dir;
    const char *runtime_lib;
    const char *corpus;
    const char *cache_dir;

    const char *github_repo;
    int file_skip_issues;

    int iters;
    int timeout_sec;
    int timeout_ms;
    int api_cases;

    int run_compat_check;
    int allow_partial;
    int rebuild_lfortran;

    int modes[MODE_COUNT];
    int lanes[LANE_COUNT];
    int policies[POLICY_COUNT];
} cfg_t;

typedef bench_cmd_result_t cmd_result_t;

typedef struct {
    double *items;
    size_t n;
    size_t cap;
} dbl_vec_t;

typedef struct {
    int ran;
    int ok;
    int rc;
    char status[64];
    char fail_reason[128];

    long long attempted;
    long long completed;
    long long skipped;
    int zero_skip_gate_met;

    double exe_wall_ms;
    double exe_compile_ms;
    double exe_run_ms;
    double exe_parse_ms;
    double exe_non_parse_ms;

    double jit_wall_ms;
    double jit_compile_ms;
    double jit_run_ms;
    double jit_parse_ms;
    double jit_non_parse_ms;

    char *summary_path;
    char *jsonl_path;
} api_provider_t;

typedef struct {
    int ran;
    int ok;
    int rc;
    char status[64];
    char fail_reason[128];

    long long attempted;
    long long completed;

    double jit_wall_ms;
    double jit_compile_ms;
    double jit_parse_ms;
    double jit_non_parse_ms;

    double llvm_wall_ms;
    double llvm_compile_ms;
    double llvm_parse_ms;
    double llvm_non_parse_ms;

    double speedup_wall;
    double speedup_non_parse;

    char *summary_path;
} ll_provider_t;

typedef struct {
    int ran;
    int ok;
    int rc;
    char status[64];
    char fail_reason[128];

    long long total_cases;
    long long wall_passed;
    long long inproc_passed;

    double wall_speedup_ratio;
    double inproc_speedup_ratio;

    char *summary_path;
} micro_provider_t;

typedef struct {
    int total;
    int tool_missing;
    int rebuild_failure;
    int compat_failure;
    int dataset_incomplete;
    int lane_execution_failed;
    int unknown;
} skip_counts_t;

static const char *k_mode_name[MODE_COUNT] = {"isel", "copy_patch", "llvm"};
static const char *k_policy_name[POLICY_COUNT] = {"direct", "ir"};
static const char *k_lane_name[LANE_COUNT] = {
    "api_exe",
    "api_jit",
    "api_e2e",
    "ll_jit",
    "ll_llvm",
    "ll_e2e",
    "ir_file",
    "micro_c"
};

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int file_executable(const char *path) {
    return path && access(path, X_OK) == 0;
}

static int dir_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n;

    if (!path || !path[0]) return -1;
    n = strlen(path);
    if (n >= sizeof(tmp)) return -1;

    memcpy(tmp, path, n + 1);
    if (tmp[n - 1] == '/') tmp[n - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p) die("out of memory");
    memcpy(p, s, n + 1);
    return p;
}

static int json_find_value_start(const char *json, const char *key, const char **out) {
    char pat[256];
    const char *p;

    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) return 0;

    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    *out = p;
    return 1;
}

static int json_get_int64(const char *json, const char *key, long long *out) {
    const char *p;
    char *endp;
    long long v;

    if (!json_find_value_start(json, key, &p)) return 0;
    errno = 0;
    v = strtoll(p, &endp, 10);
    if (p == endp || errno != 0) return 0;
    *out = v;
    return 1;
}

static int json_get_double(const char *json, const char *key, double *out) {
    const char *p;
    char *endp;
    double v;

    if (!json_find_value_start(json, key, &p)) return 0;
    errno = 0;
    v = strtod(p, &endp);
    if (p == endp || errno != 0) return 0;
    *out = v;
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out) {
    const char *p;
    if (!json_find_value_start(json, key, &p)) return 0;
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    const char *p;
    size_t n = 0;

    if (!json_find_value_start(json, key, &p)) return 0;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        if (n + 1 < out_sz) out[n++] = *p;
        p++;
    }
    if (*p != '"') return 0;
    if (out_sz > 0) out[n < out_sz ? n : out_sz - 1] = '\0';
    return 1;
}

static char *json_escape(const char *s) {
    size_t n = 0;
    const char *p;
    char *out;
    char *w;

    if (!s) return xstrdup("");

    for (p = s; *p; p++) {
        switch (*p) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
            n += 2;
            break;
        default:
            n += 1;
            break;
        }
    }

    out = (char *)malloc(n + 1);
    if (!out) die("out of memory");
    w = out;

    for (p = s; *p; p++) {
        switch (*p) {
        case '"': *w++ = '\\'; *w++ = '"'; break;
        case '\\': *w++ = '\\'; *w++ = '\\'; break;
        case '\n': *w++ = '\\'; *w++ = 'n'; break;
        case '\r': *w++ = '\\'; *w++ = 'r'; break;
        case '\t': *w++ = '\\'; *w++ = 't'; break;
        default: *w++ = *p; break;
        }
    }
    *w = '\0';
    return out;
}

static void format_iso8601_utc(char *out, size_t out_sz) {
    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, out_sz, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static int run_cmd(char *const argv[], cmd_result_t *out_res) {
    bench_run_cmd_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    return bench_run_cmd(&opts, out_res);
}

static int run_cmd_with_mode(const char *mode, char *const argv[], cmd_result_t *out_res) {
    bench_run_cmd_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    return bench_run_cmd_with_mode(mode, &opts, out_res);
}

static int host_nproc(void) {
    long n = -1;
#if defined(_SC_NPROCESSORS_ONLN)
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
#if defined(_SC_NPROCESSORS_CONF)
    if (n < 1) n = sysconf(_SC_NPROCESSORS_CONF);
#endif
    if (n < 1) return 1;
    if (n > 1024) return 1024;
    return (int)n;
}

static void dbl_vec_push(dbl_vec_t *v, double x) {
    if (v->n == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 32;
        double *tmp = (double *)realloc(v->items, ncap * sizeof(double));
        if (!tmp) die("out of memory");
        v->items = tmp;
        v->cap = ncap;
    }
    v->items[v->n++] = x;
}

static void dbl_vec_free(dbl_vec_t *v) {
    free(v->items);
    v->items = NULL;
    v->n = 0;
    v->cap = 0;
}

static int any_api_lane_selected(const cfg_t *cfg) {
    return cfg->lanes[LANE_API_EXE] || cfg->lanes[LANE_API_JIT] || cfg->lanes[LANE_API_E2E];
}

static int any_ll_lane_selected(const cfg_t *cfg) {
    return cfg->lanes[LANE_LL_JIT] || cfg->lanes[LANE_LL_LLVM] || cfg->lanes[LANE_LL_E2E] || cfg->lanes[LANE_IR_FILE];
}

static int any_micro_lane_selected(const cfg_t *cfg) {
    return cfg->lanes[LANE_MICRO_C];
}

static const char *compat_api_lane_name(const cfg_t *cfg) {
    if (cfg->lanes[LANE_API_E2E]) return "api_e2e";
    if (cfg->lanes[LANE_API_JIT]) return "api_jit";
    if (cfg->lanes[LANE_API_EXE]) return "api_exe";
    return "api_e2e";
}

static void set_all_modes(cfg_t *cfg, int v) {
    for (int i = 0; i < MODE_COUNT; i++) cfg->modes[i] = v;
}

static void set_all_policies(cfg_t *cfg, int v) {
    for (int i = 0; i < POLICY_COUNT; i++) cfg->policies[i] = v;
}

static void set_all_lanes(cfg_t *cfg, int v) {
    for (int i = 0; i < LANE_COUNT; i++) cfg->lanes[i] = v;
}

static void set_all_canonical_lanes(cfg_t *cfg) {
    set_all_lanes(cfg, 1);
    cfg->lanes[LANE_MICRO_C] = 0;
}

static void set_default_lanes(cfg_t *cfg) {
    set_all_lanes(cfg, 0);
    cfg->lanes[LANE_API_EXE] = 1;
    cfg->lanes[LANE_API_JIT] = 1;
    cfg->lanes[LANE_LL_JIT] = 1;
    cfg->lanes[LANE_LL_LLVM] = 1;
}

static int parse_policies(cfg_t *cfg, const char *text) {
    char *tmp = xstrdup(text);
    char *save = NULL;
    char *tok;

    set_all_policies(cfg, 0);
    tok = strtok_r(tmp, ",", &save);
    while (tok) {
        if (strcmp(tok, "direct") == 0) cfg->policies[POLICY_DIRECT] = 1;
        else if (strcmp(tok, "ir") == 0) cfg->policies[POLICY_IR] = 1;
        else {
            free(tmp);
            return -1;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(tmp);
    return 0;
}

static int parse_lanes(cfg_t *cfg, const char *text) {
    char *tmp = xstrdup(text);
    char *save = NULL;
    char *tok;

    set_all_lanes(cfg, 0);
    tok = strtok_r(tmp, ",", &save);
    while (tok) {
        if (strcmp(tok, "api_exe") == 0) cfg->lanes[LANE_API_EXE] = 1;
        else if (strcmp(tok, "api_jit") == 0) cfg->lanes[LANE_API_JIT] = 1;
        else if (strcmp(tok, "api_e2e") == 0) cfg->lanes[LANE_API_E2E] = 1;
        else if (strcmp(tok, "ll_jit") == 0) cfg->lanes[LANE_LL_JIT] = 1;
        else if (strcmp(tok, "ll_llvm") == 0) cfg->lanes[LANE_LL_LLVM] = 1;
        else if (strcmp(tok, "ll_e2e") == 0) cfg->lanes[LANE_LL_E2E] = 1;
        else if (strcmp(tok, "ir_file") == 0) cfg->lanes[LANE_IR_FILE] = 1;
        else if (strcmp(tok, "micro_c") == 0) cfg->lanes[LANE_MICRO_C] = 1;
        else {
            free(tmp);
            return -1;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(tmp);
    return 0;
}

static void usage(void) {
    printf("usage: bench_matrix [options]\n");
    printf("  --bench-dir PATH         output root (default: /tmp/liric_bench)\n");
    printf("  --build-dir PATH         build dir for benchmark binaries (default: build)\n");
    printf("  --manifest PATH          manifest path recorded in summary (default: tools/bench_manifest.json)\n");
    printf("  --modes LIST             comma list or 'all': isel,copy_patch,llvm\n");
    printf("  --policies LIST          comma list or 'all': direct,ir\n");
    printf("  --lanes LIST             comma list or 'all': api_exe,api_jit,api_e2e,ll_jit,ll_llvm,ll_e2e,ir_file[,micro_c]\n");
    printf("  --iters N                iterations forwarded to lane runners (default: 1)\n");
    printf("  --api-cases N            api sample cap per cell (default: 100, 0=all)\n");
    printf("  --timeout N              timeout sec for corpus compare / compat (default: 15)\n");
    printf("  --timeout-ms N           timeout ms for bench_api (default: 3000)\n");
    printf("  --skip-compat-check      do not regenerate compat artifacts\n");
    printf("  --allow-partial          report failures but return 0\n");
    printf("  --runtime-lib PATH       runtime shared library\n");
    printf("  --corpus PATH            corpus TSV\n");
    printf("  --cache-dir PATH         corpus cache directory\n");
    printf("  --lfortran PATH          lfortran LLVM binary (bench_api/compat)\n");
    printf("  --lfortran-liric PATH    lfortran WITH_LIRIC binary (bench_api)\n");
    printf("  --lfortran-build-dir PATH rebuild dir for lfortran LLVM binary (default: ../lfortran/build)\n");
    printf("  --lfortran-liric-build-dir PATH rebuild dir for lfortran WITH_LIRIC binary (only needed for split builds)\n");
    printf("  --cmake PATH             cmake executable for lfortran rebuild preflight (default: cmake)\n");
    printf("  --skip-lfortran-rebuild  disable lfortran rebuild preflight\n");
    printf("  --rebuild-lfortran       enable lfortran rebuild preflight (default)\n");
    printf("  --test-dir PATH          lfortran integration_tests directory (bench_api)\n");
    printf("  --bench-compat-check PATH\n");
    printf("  --bench-corpus-compare PATH\n");
    printf("  --bench-api PATH\n");
    printf("  --bench-tcc PATH\n");
    printf("  --probe-runner PATH\n");
    printf("  --lli-phases PATH\n");
    printf("  --file-skip-issues       auto-file GitHub issues per skip category\n");
    printf("  --github-repo OWNER/REPO target repository for issue filing\n");
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    const char *default_lfortran_llvm = "../lfortran/build/src/bin/lfortran";
    const char *default_lfortran_liric_hyphen = "../lfortran/build-liric/src/bin/lfortran";
    const char *default_lfortran_liric_underscore = "../lfortran/build_liric/src/bin/lfortran";
    const char *default_lfortran_build_liric_hyphen = "../lfortran/build-liric";
    const char *default_lfortran_build_liric_underscore = "../lfortran/build_liric";
    const char *default_runtime_dylib = "../lfortran/build/src/runtime/liblfortran_runtime.dylib";
    const char *default_runtime_so = "../lfortran/build/src/runtime/liblfortran_runtime.so";

    memset(&cfg, 0, sizeof(cfg));
    cfg.bench_dir = "/tmp/liric_bench";
    cfg.build_dir = "build";
    cfg.manifest = "tools/bench_manifest.json";
    cfg.iters = 1;
    cfg.api_cases = 100;
    cfg.timeout_sec = 15;
    cfg.timeout_ms = 3000;
    cfg.run_compat_check = 1;
    cfg.allow_partial = 0;
    cfg.rebuild_lfortran = 1;
    cfg.cmake = "cmake";
    cfg.file_skip_issues = 0;
    cfg.github_repo = getenv("GITHUB_REPOSITORY");

    cfg.lfortran = file_exists(default_lfortran_llvm) ? default_lfortran_llvm : NULL;
    cfg.lfortran_liric = file_exists(default_lfortran_liric_hyphen)
                             ? default_lfortran_liric_hyphen
                             : (file_exists(default_lfortran_liric_underscore)
                                    ? default_lfortran_liric_underscore
                                    : NULL);
    cfg.lfortran_build_dir = "../lfortran/build";
    cfg.lfortran_liric_build_dir = dir_exists(default_lfortran_build_liric_hyphen)
                                       ? default_lfortran_build_liric_hyphen
                                       : (dir_exists(default_lfortran_build_liric_underscore)
                                              ? default_lfortran_build_liric_underscore
                                              : NULL);
    cfg.runtime_lib = file_exists(default_runtime_dylib)
                          ? default_runtime_dylib
                          : (file_exists(default_runtime_so) ? default_runtime_so : NULL);

    set_all_modes(&cfg, 1);
    set_all_policies(&cfg, 1);
    set_default_lanes(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            cfg.bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--build-dir") == 0 && i + 1 < argc) {
            cfg.build_dir = argv[++i];
        } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
            cfg.manifest = argv[++i];
        } else if (strcmp(argv[i], "--modes") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "all") == 0) set_all_modes(&cfg, 1);
            else if (bench_parse_modes_csv(v, cfg.modes, MODE_COUNT) != 0) die("invalid --modes value: %s", v);
        } else if (strcmp(argv[i], "--policies") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "all") == 0) set_all_policies(&cfg, 1);
            else if (parse_policies(&cfg, v) != 0) die("invalid --policies value: %s", v);
        } else if (strcmp(argv[i], "--lanes") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "all") == 0) set_all_canonical_lanes(&cfg);
            else if (parse_lanes(&cfg, v) != 0) die("invalid --lanes value: %s", v);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            cfg.iters = atoi(argv[++i]);
            if (cfg.iters <= 0) cfg.iters = 1;
        } else if (strcmp(argv[i], "--api-cases") == 0 && i + 1 < argc) {
            cfg.api_cases = atoi(argv[++i]);
            if (cfg.api_cases < 0) cfg.api_cases = 0;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0) cfg.timeout_sec = 15;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            cfg.timeout_ms = atoi(argv[++i]);
            if (cfg.timeout_ms <= 0) cfg.timeout_ms = 3000;
        } else if (strcmp(argv[i], "--skip-compat-check") == 0) {
            cfg.run_compat_check = 0;
        } else if (strcmp(argv[i], "--allow-partial") == 0) {
            cfg.allow_partial = 1;
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            cfg.runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            cfg.corpus = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cfg.cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--lfortran") == 0 && i + 1 < argc) {
            cfg.lfortran = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-liric") == 0 && i + 1 < argc) {
            cfg.lfortran_liric = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-build-dir") == 0 && i + 1 < argc) {
            cfg.lfortran_build_dir = argv[++i];
        } else if (strcmp(argv[i], "--lfortran-liric-build-dir") == 0 && i + 1 < argc) {
            cfg.lfortran_liric_build_dir = argv[++i];
        } else if (strcmp(argv[i], "--cmake") == 0 && i + 1 < argc) {
            cfg.cmake = argv[++i];
        } else if (strcmp(argv[i], "--skip-lfortran-rebuild") == 0) {
            cfg.rebuild_lfortran = 0;
        } else if (strcmp(argv[i], "--rebuild-lfortran") == 0) {
            cfg.rebuild_lfortran = 1;
        } else if (strcmp(argv[i], "--test-dir") == 0 && i + 1 < argc) {
            cfg.test_dir = argv[++i];
        } else if (strcmp(argv[i], "--bench-compat-check") == 0 && i + 1 < argc) {
            cfg.bench_compat_check = argv[++i];
        } else if (strcmp(argv[i], "--bench-corpus-compare") == 0 && i + 1 < argc) {
            cfg.bench_corpus_compare = argv[++i];
        } else if (strcmp(argv[i], "--bench-api") == 0 && i + 1 < argc) {
            cfg.bench_api = argv[++i];
        } else if (strcmp(argv[i], "--bench-tcc") == 0 && i + 1 < argc) {
            cfg.bench_tcc = argv[++i];
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg.probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--lli-phases") == 0 && i + 1 < argc) {
            cfg.lli_phases = argv[++i];
        } else if (strcmp(argv[i], "--file-skip-issues") == 0) {
            cfg.file_skip_issues = 1;
        } else if (strcmp(argv[i], "--github-repo") == 0 && i + 1 < argc) {
            cfg.github_repo = argv[++i];
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }

    if (!cfg.bench_compat_check) cfg.bench_compat_check = bench_path_join2(cfg.build_dir, "bench_compat_check");
    if (!cfg.bench_corpus_compare) cfg.bench_corpus_compare = bench_path_join2(cfg.build_dir, "bench_corpus_compare");
    if (!cfg.bench_api) cfg.bench_api = bench_path_join2(cfg.build_dir, "bench_api");
    if (!cfg.bench_tcc) cfg.bench_tcc = bench_path_join2(cfg.build_dir, "bench_tcc");
    if (!cfg.probe_runner) cfg.probe_runner = bench_path_join2(cfg.build_dir, "liric_probe_runner");
    if (!cfg.lli_phases) cfg.lli_phases = bench_path_join2(cfg.build_dir, "bench_lli_phases");

    return cfg;
}

static int require_any(const int *bits, int n) {
    for (int i = 0; i < n; i++) {
        if (bits[i]) return 1;
    }
    return 0;
}

static int run_lfortran_rebuild_step(const cfg_t *cfg,
                                     FILE *fails,
                                     const char *lane_name,
                                     const char *build_dir,
                                     const char *missing_reason,
                                     const char *failed_reason) {
    cmd_result_t r = {0};
    char jobs_buf[32];
    char *cmd[8];
    int n = 0;

    if (!build_dir || !build_dir[0] || !dir_exists(build_dir)) {
        char *e_lane = json_escape(lane_name);
        char *e_build = json_escape(build_dir ? build_dir : "");
        fprintf(fails,
                "{\"lane\":\"%s\",\"mode\":\"all\",\"policy\":\"all\",\"baseline\":\"lfortran_llvm\","
                "\"reason\":\"%s\",\"rc\":2,\"summary\":\"%s\"}\n",
                e_lane, missing_reason, e_build);
        free(e_lane);
        free(e_build);
        return -1;
    }

    snprintf(jobs_buf, sizeof(jobs_buf), "%d", host_nproc());
    cmd[n++] = (char *)cfg->cmake;
    cmd[n++] = "--build";
    cmd[n++] = (char *)build_dir;
    cmd[n++] = "-j";
    cmd[n++] = jobs_buf;
    cmd[n++] = NULL;

    printf("[matrix] rebuild: %s\n", build_dir);
    if (run_cmd(cmd, &r) != 0 || r.rc != 0) {
        char *e_lane = json_escape(lane_name);
        char *e_build = json_escape(build_dir);
        fprintf(fails,
                "{\"lane\":\"%s\",\"mode\":\"all\",\"policy\":\"all\",\"baseline\":\"lfortran_llvm\","
                "\"reason\":\"%s\",\"rc\":%d,\"summary\":\"%s\"}\n",
                e_lane, failed_reason, r.rc, e_build);
        free(e_lane);
        free(e_build);
        return -1;
    }

    return 0;
}

static void write_failure_row(FILE *ff,
                              const char *lane,
                              const char *mode,
                              const char *policy,
                              const char *baseline,
                              const char *reason,
                              int rc,
                              const char *summary_path) {
    char *e_lane = json_escape(lane);
    char *e_mode = json_escape(mode);
    char *e_policy = json_escape(policy);
    char *e_base = json_escape(baseline);
    char *e_reason = json_escape(reason ? reason : "unknown");
    char *e_summary = json_escape(summary_path ? summary_path : "");

    fprintf(ff,
            "{\"lane\":\"%s\",\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"%s\","
            "\"reason\":\"%s\",\"rc\":%d,\"summary\":\"%s\"}\n",
            e_lane, e_mode, e_policy, e_base, e_reason, rc, e_summary);

    free(e_lane);
    free(e_mode);
    free(e_policy);
    free(e_base);
    free(e_reason);
    free(e_summary);
}

static void write_row_compat(FILE *rf,
                             const char *status,
                             long long compat_api_n,
                             long long compat_ll_n,
                             const char *bench_dir) {
    char *e_bench_dir = json_escape(bench_dir ? bench_dir : "");
    fprintf(rf,
            "{\"lane\":\"compat_check\",\"mode\":\"all\",\"policy\":\"all\",\"baseline\":\"lfortran_llvm\","
            "\"status\":\"%s\",\"compat_api_count\":%lld,\"compat_ll_count\":%lld,\"summary\":\"%s\"}\n",
            status,
            compat_api_n,
            compat_ll_n,
            e_bench_dir);
    free(e_bench_dir);
}

static void write_row_timing(FILE *rf,
                             const char *lane,
                             const char *mode,
                             const char *policy,
                             const char *baseline,
                             const char *status,
                             long long attempted,
                             long long completed,
                             long long skipped,
                             double wall_ms,
                             double compile_ms,
                             double run_ms,
                             double parse_ms,
                             double non_parse_ms,
                             const char *summary_path) {
    char *e_summary = json_escape(summary_path ? summary_path : "");
    fprintf(rf,
            "{\"lane\":\"%s\",\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"%s\","
            "\"status\":\"%s\",\"attempted\":%lld,\"completed\":%lld,\"skipped\":%lld,"
            "\"wall_ms\":%.6f,\"compile_ms\":%.6f,\"run_ms\":%.6f,\"parse_ms\":%.6f,\"non_parse_ms\":%.6f,"
            "\"summary\":\"%s\"}\n",
            lane,
            mode,
            policy,
            baseline,
            status,
            attempted,
            completed,
            skipped,
            wall_ms,
            compile_ms,
            run_ms,
            parse_ms,
            non_parse_ms,
            e_summary);
    free(e_summary);
}

static void write_row_speedup(FILE *rf,
                              const char *lane,
                              const char *mode,
                              const char *policy,
                              const char *baseline,
                              const char *status,
                              long long attempted,
                              long long completed,
                              long long skipped,
                              double wall_speedup,
                              double non_parse_speedup,
                              const char *summary_path) {
    char *e_summary = json_escape(summary_path ? summary_path : "");
    fprintf(rf,
            "{\"lane\":\"%s\",\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"%s\","
            "\"status\":\"%s\",\"attempted\":%lld,\"completed\":%lld,\"skipped\":%lld,"
            "\"wall_speedup\":%.6f,\"non_parse_speedup\":%.6f,\"summary\":\"%s\"}\n",
            lane,
            mode,
            policy,
            baseline,
            status,
            attempted,
            completed,
            skipped,
            wall_speedup,
            non_parse_speedup,
            e_summary);
    free(e_summary);
}

static long long count_lines_file(const char *path) {
    FILE *f;
    int c;
    long long lines = 0;
    int saw_data = 0;

    if (!path) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            if (saw_data) lines++;
            saw_data = 0;
        } else if (!isspace((unsigned char)c)) {
            saw_data = 1;
        }
    }
    if (saw_data) lines++;
    fclose(f);
    return lines;
}

static int copy_file_path(const char *src, const char *dst) {
    FILE *in;
    FILE *out;
    char buf[65536];
    size_t nread;

    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    while ((nread = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nread, out) != nread) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }

    if (ferror(in)) {
        fclose(in);
        fclose(out);
        return -1;
    }

    fclose(in);
    if (fclose(out) != 0) return -1;
    return 0;
}

static int prepare_ll_corpus_from_compat(const cfg_t *cfg,
                                         const char *compat_ll_path,
                                         char **out_corpus_path,
                                         char **out_cache_dir) {
    FILE *compat = NULL;
    FILE *corpus = NULL;
    char line[1024];
    char *ll_dir = NULL;
    char *corpus_path = NULL;
    char *cache_dir = NULL;
    size_t copied = 0;

    if (!cfg || !compat_ll_path || !file_exists(compat_ll_path)) return -1;
    if (!out_corpus_path || !out_cache_dir) return -1;

    ll_dir = bench_path_join2(cfg->bench_dir, "ll");
    if (!ll_dir || !dir_exists(ll_dir)) {
        free(ll_dir);
        return -1;
    }

    corpus_path = bench_path_join2(cfg->bench_dir, "corpus_from_compat.tsv");
    cache_dir = bench_path_join2(cfg->bench_dir, "cache_from_compat");
    if (!corpus_path || !cache_dir) goto fail;
    if (mkdir_p(cache_dir) != 0) goto fail;

    compat = fopen(compat_ll_path, "r");
    if (!compat) goto fail;
    corpus = fopen(corpus_path, "w");
    if (!corpus) goto fail;

    while (fgets(line, sizeof(line), compat)) {
        size_t n = strlen(line);
        char *src_ll = NULL;
        char *src_tmp = NULL;
        char *case_dir = NULL;
        char *raw_ll = NULL;

        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;

        src_tmp = bench_path_join2(ll_dir, line);
        if (!src_tmp) goto fail;
        src_ll = (char *)malloc(strlen(src_tmp) + 4);
        if (!src_ll) goto fail;
        sprintf(src_ll, "%s.ll", src_tmp);
        free(src_tmp);
        src_tmp = NULL;
        if (!file_exists(src_ll)) {
            free(src_ll);
            continue;
        }

        case_dir = bench_path_join2(cache_dir, line);
        if (!case_dir) goto fail;
        if (mkdir_p(case_dir) != 0) {
            free(src_ll);
            free(case_dir);
            goto fail;
        }
        raw_ll = bench_path_join2(case_dir, "raw.ll");
        free(case_dir);
        if (!raw_ll) {
            free(src_ll);
            goto fail;
        }
        if (copy_file_path(src_ll, raw_ll) != 0) {
            free(src_ll);
            free(raw_ll);
            goto fail;
        }

        fprintf(corpus, "%s\t%s\tcompat\n", line, line);
        copied++;
        free(src_ll);
        free(raw_ll);
    }

    fclose(compat);
    fclose(corpus);
    free(ll_dir);

    if (copied == 0) goto fail;
    *out_corpus_path = corpus_path;
    *out_cache_dir = cache_dir;
    return 0;

fail:
    if (compat) fclose(compat);
    if (corpus) fclose(corpus);
    free(ll_dir);
    free(corpus_path);
    free(cache_dir);
    return -1;
}

static int parse_api_jsonl_metrics(const char *jsonl_path, api_provider_t *p) {
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;

    dbl_vec_t exe_wall = {0};
    dbl_vec_t exe_compile = {0};
    dbl_vec_t exe_run = {0};
    dbl_vec_t exe_parse = {0};
    dbl_vec_t exe_non_parse = {0};

    dbl_vec_t jit_wall = {0};
    dbl_vec_t jit_compile = {0};
    dbl_vec_t jit_run = {0};
    dbl_vec_t jit_parse = {0};
    dbl_vec_t jit_non_parse = {0};

    if (!jsonl_path || !file_exists(jsonl_path)) return -1;

    f = fopen(jsonl_path, "r");
    if (!f) return -1;

    while ((nread = getline(&line, &cap, f)) > 0) {
        char status[64] = {0};
        double v_llvm_wall = 0.0, v_llvm_compile = 0.0, v_llvm_run = 0.0, v_llvm_parse = 0.0;
        double v_liric_wall = 0.0, v_liric_compile = 0.0, v_liric_run = 0.0, v_liric_parse = 0.0;

        (void)nread;
        if (!json_get_string(line, "status", status, sizeof(status))) continue;
        if (strcmp(status, "ok") != 0) continue;

        if (!json_get_double(line, "llvm_wall_median_ms", &v_llvm_wall)) continue;
        if (!json_get_double(line, "llvm_compile_median_ms", &v_llvm_compile)) continue;
        if (!json_get_double(line, "llvm_run_median_ms", &v_llvm_run)) continue;
        if (!json_get_double(line, "llvm_llvm_ir_median_ms", &v_llvm_parse)) continue;

        if (!json_get_double(line, "liric_wall_median_ms", &v_liric_wall)) continue;
        if (!json_get_double(line, "liric_compile_median_ms", &v_liric_compile)) continue;
        if (!json_get_double(line, "liric_run_median_ms", &v_liric_run)) continue;
        if (!json_get_double(line, "liric_llvm_ir_median_ms", &v_liric_parse)) continue;

        dbl_vec_push(&exe_wall, v_llvm_wall);
        dbl_vec_push(&exe_compile, v_llvm_compile);
        dbl_vec_push(&exe_run, v_llvm_run);
        dbl_vec_push(&exe_parse, v_llvm_parse);
        dbl_vec_push(&exe_non_parse, v_llvm_compile + v_llvm_run);

        dbl_vec_push(&jit_wall, v_liric_wall);
        dbl_vec_push(&jit_compile, v_liric_compile);
        dbl_vec_push(&jit_run, v_liric_run);
        dbl_vec_push(&jit_parse, v_liric_parse);
        dbl_vec_push(&jit_non_parse, v_liric_compile + v_liric_run);
    }

    free(line);
    fclose(f);

    if (exe_wall.n == 0 || jit_wall.n == 0) {
        dbl_vec_free(&exe_wall);
        dbl_vec_free(&exe_compile);
        dbl_vec_free(&exe_run);
        dbl_vec_free(&exe_parse);
        dbl_vec_free(&exe_non_parse);
        dbl_vec_free(&jit_wall);
        dbl_vec_free(&jit_compile);
        dbl_vec_free(&jit_run);
        dbl_vec_free(&jit_parse);
        dbl_vec_free(&jit_non_parse);
        return -1;
    }

    p->exe_wall_ms = bench_median(exe_wall.items, exe_wall.n);
    p->exe_compile_ms = bench_median(exe_compile.items, exe_compile.n);
    p->exe_run_ms = bench_median(exe_run.items, exe_run.n);
    p->exe_parse_ms = bench_median(exe_parse.items, exe_parse.n);
    p->exe_non_parse_ms = bench_median(exe_non_parse.items, exe_non_parse.n);

    p->jit_wall_ms = bench_median(jit_wall.items, jit_wall.n);
    p->jit_compile_ms = bench_median(jit_compile.items, jit_compile.n);
    p->jit_run_ms = bench_median(jit_run.items, jit_run.n);
    p->jit_parse_ms = bench_median(jit_parse.items, jit_parse.n);
    p->jit_non_parse_ms = bench_median(jit_non_parse.items, jit_non_parse.n);

    dbl_vec_free(&exe_wall);
    dbl_vec_free(&exe_compile);
    dbl_vec_free(&exe_run);
    dbl_vec_free(&exe_parse);
    dbl_vec_free(&exe_non_parse);
    dbl_vec_free(&jit_wall);
    dbl_vec_free(&jit_compile);
    dbl_vec_free(&jit_run);
    dbl_vec_free(&jit_parse);
    dbl_vec_free(&jit_non_parse);

    return 0;
}

static void run_api_provider(const cfg_t *cfg,
                             const char *mode,
                             const char *policy,
                             const char *policy_dir,
                             const char *compat_ll,
                             const char *compat_opts,
                             int need_primitives,
                             api_provider_t *p) {
    cmd_result_t r = {0};
    char iters_buf[32], timeout_buf[32], min_completed_buf[32], api_cases_buf[32];
    char *api_dir;
    char *cmd[56];
    int n = 0;

    memset(p, 0, sizeof(*p));
    p->ran = 1;
    p->rc = 0;
    strcpy(p->status, "UNKNOWN");

    api_dir = bench_path_join2(policy_dir, "api_bundle");
    if (mkdir_p(api_dir) != 0) {
        strcpy(p->fail_reason, "bench_api_dir_create_failed");
        p->rc = 1;
        free(api_dir);
        return;
    }

    p->summary_path = bench_path_join2(api_dir, "bench_api_summary.json");
    p->jsonl_path = bench_path_join2(api_dir, "bench_api.jsonl");

    if (!file_executable(cfg->bench_api)) {
        strcpy(p->fail_reason, "bench_api_missing");
        p->rc = 127;
        free(api_dir);
        return;
    }

    if (!compat_ll || !compat_opts || !file_exists(compat_ll) || !file_exists(compat_opts)) {
        strcpy(p->fail_reason, "compat_artifacts_missing");
        p->rc = 1;
        free(api_dir);
        return;
    }

    snprintf(iters_buf, sizeof(iters_buf), "%d", cfg->iters);
    snprintf(timeout_buf, sizeof(timeout_buf), "%d", cfg->timeout_ms);
    snprintf(min_completed_buf, sizeof(min_completed_buf), "%d", 1);
    snprintf(api_cases_buf, sizeof(api_cases_buf), "%d", cfg->api_cases);

    cmd[n++] = (char *)cfg->bench_api;
    cmd[n++] = "--bench-dir";
    cmd[n++] = api_dir;
    cmd[n++] = "--iters";
    cmd[n++] = iters_buf;
    cmd[n++] = "--timeout-ms";
    cmd[n++] = timeout_buf;
    cmd[n++] = "--min-completed";
    cmd[n++] = min_completed_buf;
    cmd[n++] = "--liric-policy";
    cmd[n++] = (char *)policy;
    cmd[n++] = "--compat-list";
    cmd[n++] = (char *)compat_ll;
    cmd[n++] = "--options-jsonl";
    cmd[n++] = (char *)compat_opts;

    if (cfg->api_cases > 0) {
        cmd[n++] = "--fail-sample-limit";
        cmd[n++] = api_cases_buf;
    }
    if (cfg->lfortran) {
        cmd[n++] = "--lfortran";
        cmd[n++] = (char *)cfg->lfortran;
    }
    if (cfg->lfortran_liric) {
        cmd[n++] = "--lfortran-liric";
        cmd[n++] = (char *)cfg->lfortran_liric;
    }
    if (cfg->test_dir) {
        cmd[n++] = "--test-dir";
        cmd[n++] = (char *)cfg->test_dir;
    }
    if (cfg->runtime_lib) {
        cmd[n++] = "--runtime-lib";
        cmd[n++] = (char *)cfg->runtime_lib;
    }
    cmd[n++] = NULL;

    if (run_cmd_with_mode(mode, cmd, &r) != 0 || r.rc != 0) {
        strcpy(p->fail_reason, "bench_api_failed");
        p->rc = r.rc;
        free(api_dir);
        return;
    }

    {
        char *json = bench_read_all_file(p->summary_path);
        if (!json || !json[0]) {
            strcpy(p->fail_reason, "summary_missing");
            p->rc = 1;
            free(json);
            free(api_dir);
            return;
        }

        (void)json_get_string(json, "status", p->status, sizeof(p->status));
        (void)json_get_int64(json, "attempted", &p->attempted);
        (void)json_get_int64(json, "completed", &p->completed);
        (void)json_get_int64(json, "skipped", &p->skipped);
        (void)json_get_bool(json, "zero_skip_gate_met", &p->zero_skip_gate_met);
        free(json);
    }

    p->ok = (strcmp(p->status, "OK") == 0 &&
             p->attempted > 0 &&
             p->completed > 0);

    if (!p->ok) {
        strcpy(p->fail_reason, "api_lane_incomplete");
        p->rc = 1;
        free(api_dir);
        return;
    }

    if (need_primitives) {
        if (parse_api_jsonl_metrics(p->jsonl_path, p) != 0) {
            strcpy(p->fail_reason, "bench_api_jsonl_missing");
            p->ok = 0;
            p->rc = 1;
            free(api_dir);
            return;
        }
    }

    free(api_dir);
}

static void run_ll_provider(const cfg_t *cfg,
                            const char *mode,
                            const char *policy,
                            const char *policy_dir,
                            ll_provider_t *p) {
    cmd_result_t r = {0};
    char iters_buf[32], timeout_buf[32];
    char *ll_dir;
    char *cmd[44];
    int n = 0;

    memset(p, 0, sizeof(*p));
    p->ran = 1;
    p->rc = 0;
    strcpy(p->status, "UNKNOWN");

    ll_dir = bench_path_join2(policy_dir, "ll_bundle");
    if (mkdir_p(ll_dir) != 0) {
        strcpy(p->fail_reason, "bench_corpus_compare_dir_create_failed");
        p->rc = 1;
        free(ll_dir);
        return;
    }

    p->summary_path = bench_path_join2(ll_dir, "bench_corpus_compare_summary.json");

    if (!file_executable(cfg->bench_corpus_compare)) {
        strcpy(p->fail_reason, "bench_corpus_compare_missing");
        p->rc = 127;
        free(ll_dir);
        return;
    }
    if (!file_executable(cfg->probe_runner)) {
        strcpy(p->fail_reason, "liric_probe_runner_missing");
        p->rc = 127;
        free(ll_dir);
        return;
    }
    if (!file_executable(cfg->lli_phases)) {
        strcpy(p->fail_reason, "bench_lli_phases_missing");
        p->rc = 127;
        free(ll_dir);
        return;
    }

    snprintf(iters_buf, sizeof(iters_buf), "%d", cfg->iters);
    snprintf(timeout_buf, sizeof(timeout_buf), "%d", cfg->timeout_sec);

    cmd[n++] = (char *)cfg->bench_corpus_compare;
    cmd[n++] = "--bench-dir";
    cmd[n++] = ll_dir;
    cmd[n++] = "--iters";
    cmd[n++] = iters_buf;
    cmd[n++] = "--timeout";
    cmd[n++] = timeout_buf;
    cmd[n++] = "--policy";
    cmd[n++] = (char *)policy;
    cmd[n++] = "--probe-runner";
    cmd[n++] = (char *)cfg->probe_runner;
    cmd[n++] = "--lli-phases";
    cmd[n++] = (char *)cfg->lli_phases;

    if (cfg->runtime_lib) {
        cmd[n++] = "--runtime-lib";
        cmd[n++] = (char *)cfg->runtime_lib;
    }
    if (cfg->corpus) {
        cmd[n++] = "--corpus";
        cmd[n++] = (char *)cfg->corpus;
    }
    if (cfg->cache_dir) {
        cmd[n++] = "--cache-dir";
        cmd[n++] = (char *)cfg->cache_dir;
    }
    cmd[n++] = NULL;

    if (run_cmd_with_mode(mode, cmd, &r) != 0 || r.rc != 0) {
        strcpy(p->fail_reason, "bench_corpus_compare_failed");
        p->rc = r.rc;
        free(ll_dir);
        return;
    }

    {
        char *json = bench_read_all_file(p->summary_path);
        if (!json || !json[0]) {
            strcpy(p->fail_reason, "summary_missing");
            p->rc = 1;
            free(json);
            free(ll_dir);
            return;
        }

        (void)json_get_string(json, "status", p->status, sizeof(p->status));
        (void)json_get_int64(json, "attempted_tests", &p->attempted);
        (void)json_get_int64(json, "completed_tests", &p->completed);

        (void)json_get_double(json, "liric_total_materialized_median_ms", &p->jit_wall_ms);
        (void)json_get_double(json, "liric_compile_materialized_median_ms", &p->jit_compile_ms);
        (void)json_get_double(json, "liric_parse_median_ms", &p->jit_parse_ms);
        p->jit_non_parse_ms = p->jit_compile_ms;

        (void)json_get_double(json, "llvm_total_materialized_median_ms", &p->llvm_wall_ms);
        (void)json_get_double(json, "llvm_compile_materialized_median_ms", &p->llvm_compile_ms);
        (void)json_get_double(json, "llvm_parse_input_median_ms", &p->llvm_parse_ms);
        p->llvm_non_parse_ms = p->llvm_compile_ms;

        (void)json_get_double(json, "total_materialized_speedup_median", &p->speedup_wall);
        (void)json_get_double(json, "compile_materialized_speedup_median", &p->speedup_non_parse);
        free(json);
    }

    p->ok = ((strcmp(p->status, "OK") == 0 || strcmp(p->status, "PARTIAL") == 0) &&
             p->attempted > 0 &&
             p->completed > 0);
    if (!p->ok) {
        strcpy(p->fail_reason, "ll_lane_incomplete");
        p->rc = 1;
    }

    free(ll_dir);
}

static void run_micro_provider(const cfg_t *cfg,
                               const char *mode,
                               const char *policy,
                               const char *policy_dir,
                               micro_provider_t *p) {
    cmd_result_t r = {0};
    char iters_buf[32];
    char *micro_dir;
    char *cmd[20];
    int n = 0;

    memset(p, 0, sizeof(*p));
    p->ran = 1;
    p->rc = 0;
    strcpy(p->status, "UNKNOWN");

    micro_dir = bench_path_join2(policy_dir, "micro_bundle");
    if (mkdir_p(micro_dir) != 0) {
        strcpy(p->fail_reason, "bench_tcc_dir_create_failed");
        p->rc = 1;
        free(micro_dir);
        return;
    }

    p->summary_path = bench_path_join2(micro_dir, "bench_tcc_summary.json");

    if (!file_executable(cfg->bench_tcc)) {
        strcpy(p->fail_reason, "bench_tcc_missing");
        p->rc = 127;
        free(micro_dir);
        return;
    }

    snprintf(iters_buf, sizeof(iters_buf), "%d", cfg->iters);
    cmd[n++] = (char *)cfg->bench_tcc;
    cmd[n++] = "--iters";
    cmd[n++] = iters_buf;
    cmd[n++] = "--policy";
    cmd[n++] = (char *)policy;
    cmd[n++] = "--bench-dir";
    cmd[n++] = micro_dir;
    cmd[n++] = NULL;

    if (run_cmd_with_mode(mode, cmd, &r) != 0 || r.rc != 0) {
        strcpy(p->fail_reason, "bench_tcc_failed");
        p->rc = r.rc;
        free(micro_dir);
        return;
    }

    {
        char *json = bench_read_all_file(p->summary_path);
        if (!json || !json[0]) {
            strcpy(p->fail_reason, "summary_missing");
            p->rc = 1;
            free(json);
            free(micro_dir);
            return;
        }

        (void)json_get_string(json, "status", p->status, sizeof(p->status));
        (void)json_get_int64(json, "total_cases", &p->total_cases);
        (void)json_get_int64(json, "wall_passed", &p->wall_passed);
        (void)json_get_int64(json, "inproc_passed", &p->inproc_passed);
        (void)json_get_double(json, "wall_speedup_ratio", &p->wall_speedup_ratio);
        (void)json_get_double(json, "inproc_speedup_ratio", &p->inproc_speedup_ratio);
        free(json);
    }

    p->ok = (strcmp(p->status, "OK") == 0 &&
             p->total_cases > 0 &&
             p->wall_passed == p->total_cases &&
             p->inproc_passed == p->total_cases);
    if (!p->ok) {
        strcpy(p->fail_reason, "micro_lane_incomplete");
        p->rc = 1;
    }

    free(micro_dir);
}

static void classify_reason(const char *reason, const char **category, const char **fix_contract) {
    if (!reason) reason = "unknown";

    if (strstr(reason, "_missing") != NULL || strstr(reason, "binary_missing") != NULL) {
        *category = "tool_missing";
        *fix_contract = "Provide/install missing binary or artifact path and re-run the matrix.";
        return;
    }
    if (strstr(reason, "rebuild") != NULL) {
        *category = "rebuild_failure";
        *fix_contract = "Fix lfortran build preflight for both LLVM and WITH_LIRIC binaries.";
        return;
    }
    if (strstr(reason, "compat") != NULL) {
        *category = "compat_failure";
        *fix_contract = "Restore compat_check generation and ensure compat lists/options artifacts exist.";
        return;
    }
    if (strstr(reason, "incomplete") != NULL || strstr(reason, "EMPTY") != NULL) {
        *category = "dataset_incomplete";
        *fix_contract = "Increase completed coverage to attempted==completed with zero skips for strict lanes.";
        return;
    }
    if (strstr(reason, "failed") != NULL) {
        *category = "lane_execution_failed";
        *fix_contract = "Fix lane execution error and ensure summary artifacts are emitted with status OK.";
        return;
    }

    *category = "unknown";
    *fix_contract = "Inspect failure reason and define a concrete fix contract for this skip class.";
}

static int category_index(const char *category) {
    if (strcmp(category, "tool_missing") == 0) return 0;
    if (strcmp(category, "rebuild_failure") == 0) return 1;
    if (strcmp(category, "compat_failure") == 0) return 2;
    if (strcmp(category, "dataset_incomplete") == 0) return 3;
    if (strcmp(category, "lane_execution_failed") == 0) return 4;
    return 5;
}

static int build_skip_artifacts(const char *fails_path,
                                const char *skips_path,
                                skip_counts_t *counts_out) {
    FILE *in;
    FILE *out;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;
    skip_counts_t c;

    memset(&c, 0, sizeof(c));

    in = fopen(fails_path, "r");
    if (!in) return -1;
    out = fopen(skips_path, "w");
    if (!out) {
        fclose(in);
        return -1;
    }

    while ((nread = getline(&line, &cap, in)) > 0) {
        char lane[64] = {0};
        char mode[64] = {0};
        char policy[64] = {0};
        char baseline[64] = {0};
        char reason[256] = {0};
        char summary[PATH_MAX] = {0};
        long long rc = 0;
        const char *category;
        const char *fix_contract;
        int idx;
        char *e_summary;
        char *e_fix;

        (void)nread;
        (void)json_get_string(line, "lane", lane, sizeof(lane));
        (void)json_get_string(line, "mode", mode, sizeof(mode));
        (void)json_get_string(line, "policy", policy, sizeof(policy));
        (void)json_get_string(line, "baseline", baseline, sizeof(baseline));
        (void)json_get_string(line, "reason", reason, sizeof(reason));
        (void)json_get_string(line, "summary", summary, sizeof(summary));
        (void)json_get_int64(line, "rc", &rc);

        classify_reason(reason, &category, &fix_contract);
        idx = category_index(category);
        c.total++;
        if (idx == 0) c.tool_missing++;
        else if (idx == 1) c.rebuild_failure++;
        else if (idx == 2) c.compat_failure++;
        else if (idx == 3) c.dataset_incomplete++;
        else if (idx == 4) c.lane_execution_failed++;
        else c.unknown++;

        e_summary = json_escape(summary);
        e_fix = json_escape(fix_contract);
        fprintf(out,
                "{\"lane\":\"%s\",\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"%s\","
                "\"reason\":\"%s\",\"rc\":%lld,\"category\":\"%s\",\"fix_contract\":\"%s\",\"summary\":\"%s\"}\n",
                lane,
                mode,
                policy,
                baseline,
                reason,
                rc,
                category,
                e_fix,
                e_summary);
        free(e_summary);
        free(e_fix);
    }

    free(line);
    fclose(in);
    fclose(out);

    *counts_out = c;
    return 0;
}

static int gh_issue_exists(const char *repo, const char *title) {
    cmd_result_t r = {0};
    char *cmd[14];
    int n = 0;

    cmd[n++] = "gh";
    cmd[n++] = "issue";
    cmd[n++] = "list";
    cmd[n++] = "--repo";
    cmd[n++] = (char *)repo;
    cmd[n++] = "--state";
    cmd[n++] = "open";
    cmd[n++] = "--search";
    cmd[n++] = (char *)title;
    cmd[n++] = "--limit";
    cmd[n++] = "20";
    cmd[n++] = "--json";
    cmd[n++] = "title";
    cmd[n++] = NULL;

    if (run_cmd(cmd, &r) != 0 || r.rc != 0) return 0;
    if (!r.stdout_text) return 0;
    return strstr(r.stdout_text, title) != NULL;
}

static void maybe_file_skip_issues(const cfg_t *cfg, const skip_counts_t *counts) {
    int gh_ok = 0;
    cmd_result_t rv = {0};
    char *probe_cmd[] = {"gh", "--version", NULL};

    const char *cats[] = {
        "tool_missing",
        "rebuild_failure",
        "compat_failure",
        "dataset_incomplete",
        "lane_execution_failed",
        "unknown"
    };
    const int vals[] = {
        counts->tool_missing,
        counts->rebuild_failure,
        counts->compat_failure,
        counts->dataset_incomplete,
        counts->lane_execution_failed,
        counts->unknown
    };

    if (!cfg->file_skip_issues || counts->total == 0) return;
    if (!cfg->github_repo || !cfg->github_repo[0]) {
        fprintf(stderr, "[matrix] skip-issue filing requested but no --github-repo provided\n");
        return;
    }

    if (run_cmd(probe_cmd, &rv) == 0 && rv.rc == 0) gh_ok = 1;
    if (!gh_ok) {
        fprintf(stderr, "[matrix] skip-issue filing requested but gh CLI is unavailable\n");
        return;
    }

    for (size_t i = 0; i < sizeof(cats) / sizeof(cats[0]); i++) {
        char title[256];
        char body[1024];
        char *cmd[14];
        int n = 0;

        if (vals[i] <= 0) continue;

        snprintf(title, sizeof(title), "bench_matrix skip category: %s", cats[i]);
        if (gh_issue_exists(cfg->github_repo, title)) continue;

        snprintf(body,
                 sizeof(body),
                 "Auto-filed from bench_matrix.\\n\\n"
                 "Category: %s\\n"
                 "Current skipped cells: %d\\n\\n"
                 "Expected contract:\\n"
                 "- eliminate this skip category from strict matrix runs\\n"
                 "- keep attempted==completed for required lanes\\n",
                 cats[i],
                 vals[i]);

        cmd[n++] = "gh";
        cmd[n++] = "issue";
        cmd[n++] = "create";
        cmd[n++] = "--repo";
        cmd[n++] = (char *)cfg->github_repo;
        cmd[n++] = "--title";
        cmd[n++] = title;
        cmd[n++] = "--body";
        cmd[n++] = body;
        cmd[n++] = NULL;

        {
            cmd_result_t r = {0};
            if (run_cmd(cmd, &r) != 0 || r.rc != 0) {
                fprintf(stderr, "[matrix] failed to file issue for category %s\n", cats[i]);
            } else {
                printf("[matrix] filed issue: %s\n", title);
            }
        }
    }
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);

    FILE *rows = NULL;
    FILE *fails = NULL;

    char *rows_path = NULL;
    char *fails_path = NULL;
    char *summary_path = NULL;
    char *skips_path = NULL;

    char *compat_ll = NULL;
    char *compat_api = NULL;
    char *compat_opts = NULL;
    char *auto_corpus = NULL;
    char *auto_cache = NULL;

    int cells_attempted = 0;
    int cells_ok = 0;
    int cells_failed = 0;

    int compat_ok = 1;
    int ran_compat = 0;

    skip_counts_t skip_counts;

    if (!require_any(cfg.lanes, LANE_COUNT)) die("no lanes selected");
    if (!require_any(cfg.modes, MODE_COUNT)) die("no modes selected");
    if (!require_any(cfg.policies, POLICY_COUNT)) die("no policies selected");

    if (cfg.manifest && !file_exists(cfg.manifest)) die("manifest missing: %s", cfg.manifest);

    if (mkdir_p(cfg.bench_dir) != 0) die("failed to create bench dir: %s", cfg.bench_dir);

    rows_path = bench_path_join2(cfg.bench_dir, "matrix_rows.jsonl");
    fails_path = bench_path_join2(cfg.bench_dir, "matrix_failures.jsonl");
    summary_path = bench_path_join2(cfg.bench_dir, "matrix_summary.json");
    skips_path = bench_path_join2(cfg.bench_dir, "matrix_skips.jsonl");

    compat_ll = bench_path_join2(cfg.bench_dir, "compat_ll.txt");
    compat_api = bench_path_join2(cfg.bench_dir, "compat_api.txt");
    compat_opts = bench_path_join2(cfg.bench_dir, "compat_ll_options.jsonl");

    rows = fopen(rows_path, "w");
    if (!rows) die("failed to open rows output: %s", rows_path);
    fails = fopen(fails_path, "w");
    if (!fails) die("failed to open failures output: %s", fails_path);

    if (any_api_lane_selected(&cfg)) {
        const char *api_lane = compat_api_lane_name(&cfg);
        if (!cfg.lfortran || !cfg.lfortran[0] || !file_exists(cfg.lfortran)) {
            write_failure_row(fails, api_lane, "all", "all", "lfortran_llvm", "lfortran_binary_missing", 127,
                              cfg.lfortran ? cfg.lfortran : "");
            compat_ok = 0;
        }
        if (!cfg.lfortran_liric || !cfg.lfortran_liric[0] || !file_exists(cfg.lfortran_liric)) {
            write_failure_row(fails, api_lane, "all", "all", "lfortran_llvm", "lfortran_liric_binary_missing", 127,
                              cfg.lfortran_liric ? cfg.lfortran_liric : "");
            compat_ok = 0;
        }

        if (cfg.rebuild_lfortran && compat_ok) {
            if (run_lfortran_rebuild_step(&cfg,
                                          fails,
                                          api_lane,
                                          cfg.lfortran_build_dir,
                                          "lfortran_build_dir_missing",
                                          "lfortran_llvm_rebuild_failed") != 0) {
                compat_ok = 0;
            }

            if (cfg.lfortran_liric_build_dir &&
                cfg.lfortran_liric_build_dir[0] &&
                (!cfg.lfortran_build_dir ||
                 strcmp(cfg.lfortran_build_dir, cfg.lfortran_liric_build_dir) != 0)) {
                if (run_lfortran_rebuild_step(&cfg,
                                              fails,
                                              api_lane,
                                              cfg.lfortran_liric_build_dir,
                                              "lfortran_liric_build_dir_missing",
                                              "lfortran_liric_rebuild_failed") != 0) {
                    compat_ok = 0;
                }
            } else if ((!cfg.lfortran_liric_build_dir || !cfg.lfortran_liric_build_dir[0]) &&
                       cfg.lfortran_liric &&
                       cfg.lfortran &&
                       strcmp(cfg.lfortran_liric, cfg.lfortran) != 0) {
                write_failure_row(fails,
                                  api_lane,
                                  "all",
                                  "all",
                                  "lfortran_llvm",
                                  "lfortran_liric_build_dir_missing",
                                  2,
                                  "");
                compat_ok = 0;
            }
        }

        if (cfg.run_compat_check) {
            cmd_result_t r = {0};
            char timeout_buf[32];
            char *cmd[24];
            int n = 0;

            if (!compat_ok) {
                write_row_compat(rows, "FAILED", 0, 0, cfg.bench_dir);
                ran_compat = 1;
            } else if (!file_executable(cfg.bench_compat_check)) {
                compat_ok = 0;
                write_failure_row(fails,
                                  api_lane,
                                  "all",
                                  "all",
                                  "lfortran_llvm",
                                  "bench_compat_check_missing",
                                  127,
                                  cfg.bench_compat_check);
            } else {
                snprintf(timeout_buf, sizeof(timeout_buf), "%d", cfg.timeout_sec);
                cmd[n++] = (char *)cfg.bench_compat_check;
                cmd[n++] = "--bench-dir";
                cmd[n++] = (char *)cfg.bench_dir;
                cmd[n++] = "--timeout";
                cmd[n++] = timeout_buf;
                if (cfg.runtime_lib) {
                    cmd[n++] = "--runtime-lib";
                    cmd[n++] = (char *)cfg.runtime_lib;
                }
                if (cfg.lfortran) {
                    cmd[n++] = "--lfortran";
                    cmd[n++] = (char *)cfg.lfortran;
                }
                cmd[n++] = NULL;

                printf("[matrix] compat_check\n");
                if (run_cmd(cmd, &r) != 0 || r.rc != 0) {
                    compat_ok = 0;
                    write_row_compat(rows, "FAILED", 0, 0, cfg.bench_dir);
                    write_failure_row(fails,
                                      api_lane,
                                      "all",
                                      "all",
                                      "lfortran_llvm",
                                      "bench_compat_check_failed",
                                      r.rc,
                                      cfg.bench_compat_check);
                } else if (!file_exists(compat_ll) || !file_exists(compat_opts)) {
                    compat_ok = 0;
                    write_row_compat(rows, "FAILED", 0, 0, cfg.bench_dir);
                    write_failure_row(fails,
                                      api_lane,
                                      "all",
                                      "all",
                                      "lfortran_llvm",
                                      "compat_artifacts_missing",
                                      1,
                                      cfg.bench_dir);
                } else {
                    write_row_compat(rows,
                                     "OK",
                                     count_lines_file(compat_api),
                                     count_lines_file(compat_ll),
                                     cfg.bench_dir);
                }
                ran_compat = 1;
            }
        }
    }

    if (any_ll_lane_selected(&cfg) &&
        (!cfg.corpus || !cfg.cache_dir || !file_exists(cfg.corpus) || !dir_exists(cfg.cache_dir))) {
        if (prepare_ll_corpus_from_compat(&cfg, compat_ll, &auto_corpus, &auto_cache) == 0) {
            cfg.corpus = auto_corpus;
            cfg.cache_dir = auto_cache;
            printf("[matrix] ll corpus bootstrap: corpus=%s cache=%s\n", cfg.corpus, cfg.cache_dir);
        } else {
            fprintf(stderr,
                    "[matrix] failed to bootstrap ll corpus from compat artifacts; "
                    "ll lanes may fail without --corpus/--cache-dir\n");
        }
    }

    for (int mi = 0; mi < MODE_COUNT; mi++) {
        const char *mode;
        if (!cfg.modes[mi]) continue;
        mode = k_mode_name[mi];

        for (int pi = 0; pi < POLICY_COUNT; pi++) {
            const char *policy;
            char *mode_dir;
            char *policy_dir;

            api_provider_t ap = {0};
            ll_provider_t ll = {0};
            micro_provider_t micro = {0};

            int want_api = any_api_lane_selected(&cfg);
            int want_api_primitives = cfg.lanes[LANE_API_EXE] || cfg.lanes[LANE_API_JIT];
            int want_ll = any_ll_lane_selected(&cfg);
            int want_micro = any_micro_lane_selected(&cfg);

            if (!cfg.policies[pi]) continue;
            policy = k_policy_name[pi];

            mode_dir = bench_path_join2(cfg.bench_dir, mode);
            policy_dir = bench_path_join2(mode_dir, policy);
            if (mkdir_p(policy_dir) != 0) die("failed to create policy dir: %s", policy_dir);

            if (want_api) {
                if (!compat_ok) {
                    memset(&ap, 0, sizeof(ap));
                    ap.ran = 1;
                    ap.ok = 0;
                    ap.rc = 1;
                    strcpy(ap.status, "FAILED");
                    strcpy(ap.fail_reason, "compat_check_unavailable");
                    ap.summary_path = xstrdup(cfg.bench_dir);
                } else {
                    run_api_provider(&cfg, mode, policy, policy_dir, compat_ll, compat_opts, want_api_primitives, &ap);
                }
            }

            if (want_ll) {
                run_ll_provider(&cfg, mode, policy, policy_dir, &ll);
            }

            if (want_micro) {
                run_micro_provider(&cfg, mode, policy, policy_dir, &micro);
            }

            for (int li = 0; li < LANE_COUNT; li++) {
                const char *lane = k_lane_name[li];

                if (!cfg.lanes[li]) continue;

                cells_attempted++;
                printf("[matrix] mode=%s policy=%s lane=%s\n", mode, policy, lane);

                if (li == LANE_API_EXE) {
                    if (!ap.ran || !ap.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "lfortran_llvm",
                                          ap.fail_reason[0] ? ap.fail_reason : "api_lane_unavailable",
                                          ap.rc,
                                          ap.summary_path ? ap.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    write_row_timing(rows,
                                     lane,
                                     mode,
                                     policy,
                                     "lfortran_llvm",
                                     ap.status,
                                     ap.attempted,
                                     ap.completed,
                                     ap.skipped,
                                     ap.exe_wall_ms,
                                     ap.exe_compile_ms,
                                     ap.exe_run_ms,
                                     ap.exe_parse_ms,
                                     ap.exe_non_parse_ms,
                                     ap.summary_path);
                    cells_ok++;
                } else if (li == LANE_API_JIT) {
                    if (!ap.ran || !ap.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "lfortran_llvm",
                                          ap.fail_reason[0] ? ap.fail_reason : "api_lane_unavailable",
                                          ap.rc,
                                          ap.summary_path ? ap.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    write_row_timing(rows,
                                     lane,
                                     mode,
                                     policy,
                                     "lfortran_llvm",
                                     ap.status,
                                     ap.attempted,
                                     ap.completed,
                                     ap.skipped,
                                     ap.jit_wall_ms,
                                     ap.jit_compile_ms,
                                     ap.jit_run_ms,
                                     ap.jit_parse_ms,
                                     ap.jit_non_parse_ms,
                                     ap.summary_path);
                    cells_ok++;
                } else if (li == LANE_API_E2E) {
                    if (!ap.ran || !ap.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "lfortran_llvm",
                                          ap.fail_reason[0] ? ap.fail_reason : "api_lane_unavailable",
                                          ap.rc,
                                          ap.summary_path ? ap.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    {
                        double wall_speedup = (ap.jit_wall_ms > 0.0) ? ap.exe_wall_ms / ap.jit_wall_ms : 0.0;
                        double np_speedup = (ap.jit_non_parse_ms > 0.0) ? ap.exe_non_parse_ms / ap.jit_non_parse_ms : 0.0;
                        write_row_speedup(rows,
                                          lane,
                                          mode,
                                          policy,
                                          "lfortran_llvm",
                                          ap.status,
                                          ap.attempted,
                                          ap.completed,
                                          ap.skipped,
                                          wall_speedup,
                                          np_speedup,
                                          ap.summary_path);
                    }
                    cells_ok++;
                } else if (li == LANE_LL_JIT) {
                    if (!ll.ran || !ll.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "llvm",
                                          ll.fail_reason[0] ? ll.fail_reason : "ll_lane_unavailable",
                                          ll.rc,
                                          ll.summary_path ? ll.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    write_row_timing(rows,
                                     lane,
                                     mode,
                                     policy,
                                     "llvm",
                                     ll.status,
                                     ll.attempted,
                                     ll.completed,
                                     0,
                                     ll.jit_wall_ms,
                                     ll.jit_compile_ms,
                                     -1.0,
                                     ll.jit_parse_ms,
                                     ll.jit_non_parse_ms,
                                     ll.summary_path);
                    cells_ok++;
                } else if (li == LANE_LL_LLVM) {
                    if (!ll.ran || !ll.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "llvm",
                                          ll.fail_reason[0] ? ll.fail_reason : "ll_lane_unavailable",
                                          ll.rc,
                                          ll.summary_path ? ll.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    write_row_timing(rows,
                                     lane,
                                     mode,
                                     policy,
                                     "llvm",
                                     ll.status,
                                     ll.attempted,
                                     ll.completed,
                                     0,
                                     ll.llvm_wall_ms,
                                     ll.llvm_compile_ms,
                                     -1.0,
                                     ll.llvm_parse_ms,
                                     ll.llvm_non_parse_ms,
                                     ll.summary_path);
                    cells_ok++;
                } else if (li == LANE_LL_E2E || li == LANE_IR_FILE) {
                    if (!ll.ran || !ll.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "llvm",
                                          ll.fail_reason[0] ? ll.fail_reason : "ll_lane_unavailable",
                                          ll.rc,
                                          ll.summary_path ? ll.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    write_row_speedup(rows,
                                      lane,
                                      mode,
                                      policy,
                                      "llvm",
                                      ll.status,
                                      ll.attempted,
                                      ll.completed,
                                      0,
                                      ll.speedup_wall,
                                      ll.speedup_non_parse,
                                      ll.summary_path);
                    cells_ok++;
                } else if (li == LANE_MICRO_C) {
                    if (!micro.ran || !micro.ok) {
                        write_failure_row(fails,
                                          lane,
                                          mode,
                                          policy,
                                          "tcc",
                                          micro.fail_reason[0] ? micro.fail_reason : "micro_lane_unavailable",
                                          micro.rc,
                                          micro.summary_path ? micro.summary_path : "");
                        cells_failed++;
                        continue;
                    }
                    write_row_speedup(rows,
                                      lane,
                                      mode,
                                      policy,
                                      "tcc",
                                      micro.status,
                                      micro.total_cases,
                                      micro.wall_passed,
                                      0,
                                      micro.wall_speedup_ratio,
                                      micro.inproc_speedup_ratio,
                                      micro.summary_path);
                    cells_ok++;
                }
            }

            free(mode_dir);
            free(policy_dir);
            free(ap.summary_path);
            free(ap.jsonl_path);
            free(ll.summary_path);
            free(micro.summary_path);
        }
    }

    fclose(rows);
    fclose(fails);

    if (build_skip_artifacts(fails_path, skips_path, &skip_counts) != 0) {
        memset(&skip_counts, 0, sizeof(skip_counts));
    }

    maybe_file_skip_issues(&cfg, &skip_counts);

    {
        FILE *sf = fopen(summary_path, "w");
        char ts[64] = {0};
        char status[16];

        if (!sf) die("failed to write summary: %s", summary_path);

        format_iso8601_utc(ts, sizeof(ts));
        strcpy(status, (cells_attempted > 0 && cells_failed == 0) ? "OK" : "FAILED");

        fprintf(sf, "{\n");
        fprintf(sf, "  \"schema_version\": 2,\n");
        fprintf(sf, "  \"generated_at_utc\": \"%s\",\n", ts);

        {
            char *e_bench_dir = json_escape(cfg.bench_dir);
            char *e_manifest = json_escape(cfg.manifest ? cfg.manifest : "");
            char *e_rows = json_escape(rows_path);
            char *e_fails = json_escape(fails_path);
            char *e_skips = json_escape(skips_path);
            char *e_repo = json_escape(cfg.github_repo ? cfg.github_repo : "");

            fprintf(sf, "  \"bench_dir\": \"%s\",\n", e_bench_dir);
            fprintf(sf, "  \"manifest\": \"%s\",\n", e_manifest);
            fprintf(sf, "  \"rows_jsonl\": \"%s\",\n", e_rows);
            fprintf(sf, "  \"failures_jsonl\": \"%s\",\n", e_fails);
            fprintf(sf, "  \"skips_jsonl\": \"%s\",\n", e_skips);
            fprintf(sf, "  \"github_repo\": \"%s\",\n", e_repo);

            free(e_bench_dir);
            free(e_manifest);
            free(e_rows);
            free(e_fails);
            free(e_skips);
            free(e_repo);
        }

        fprintf(sf, "  \"status\": \"%s\",\n", status);
        fprintf(sf, "  \"cells_attempted\": %d,\n", cells_attempted);
        fprintf(sf, "  \"cells_ok\": %d,\n", cells_ok);
        fprintf(sf, "  \"cells_failed\": %d,\n", cells_failed);
        fprintf(sf, "  \"ran_compat_check\": %s,\n", ran_compat ? "true" : "false");
        fprintf(sf, "  \"compat_ok\": %s,\n", compat_ok ? "true" : "false");
        fprintf(sf, "  \"skip_issue_filing_enabled\": %s,\n", cfg.file_skip_issues ? "true" : "false");

        fprintf(sf, "  \"skip_categories\": {\n");
        fprintf(sf, "    \"total\": %d,\n", skip_counts.total);
        fprintf(sf, "    \"tool_missing\": %d,\n", skip_counts.tool_missing);
        fprintf(sf, "    \"rebuild_failure\": %d,\n", skip_counts.rebuild_failure);
        fprintf(sf, "    \"compat_failure\": %d,\n", skip_counts.compat_failure);
        fprintf(sf, "    \"dataset_incomplete\": %d,\n", skip_counts.dataset_incomplete);
        fprintf(sf, "    \"lane_execution_failed\": %d,\n", skip_counts.lane_execution_failed);
        fprintf(sf, "    \"unknown\": %d\n", skip_counts.unknown);
        fprintf(sf, "  }\n");

        fprintf(sf, "}\n");
        fclose(sf);
    }

    printf("[matrix] summary: %s\n", summary_path);
    printf("[matrix] rows:    %s\n", rows_path);
    printf("[matrix] fails:   %s\n", fails_path);
    printf("[matrix] skips:   %s\n", skips_path);
    printf("[matrix] cells: attempted=%d ok=%d failed=%d\n", cells_attempted, cells_ok, cells_failed);

    free(rows_path);
    free(fails_path);
    free(summary_path);
    free(skips_path);
    free(compat_ll);
    free(compat_api);
    free(compat_opts);
    free(auto_corpus);
    free(auto_cache);

    if (cells_attempted == 0) {
        fprintf(stderr, "no matrix cells attempted\n");
        return 1;
    }
    if (cells_failed > 0 && !cfg.allow_partial) return 1;
    return 0;
}
