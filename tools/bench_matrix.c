// Unified benchmark matrix runner.
//
// Executes benchmark lanes across compile modes and emits one consolidated result
// schema with strict hard-fail accounting.
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
    LANE_IR_FILE = 0,
    LANE_API_E2E = 1,
    LANE_MICRO_C = 2,
    LANE_COUNT = 3
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

static const char *k_mode_name[MODE_COUNT] = {"isel", "copy_patch", "llvm"};
static const char *k_lane_name[LANE_COUNT] = {"ir_file", "api_e2e", "micro_c"};
static const char *k_policy_name[POLICY_COUNT] = {"direct", "ir"};

static void write_failure_row(FILE *ff,
                              const char *lane,
                              const char *mode,
                              const char *policy,
                              const char *baseline,
                              const char *reason,
                              int rc,
                              const char *summary_path);

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

    if (!path || !path[0])
        return -1;

    n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;

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

static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (!s)
        return NULL;
    n = strlen(s);
    p = (char *)malloc(n + 1);
    if (!p)
        die("out of memory");
    memcpy(p, s, n + 1);
    return p;
}

static int json_find_value_start(const char *json, const char *key, const char **out) {
    char pat[256];
    const char *p;

    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat))
        return 0;

    p = strstr(json, pat);
    if (!p)
        return 0;
    p += strlen(pat);
    while (*p && *p != ':')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    *out = p;
    return 1;
}

static int json_get_int64(const char *json, const char *key, long long *out) {
    const char *p;
    char *endp;
    long long v;

    if (!json_find_value_start(json, key, &p))
        return 0;
    errno = 0;
    v = strtoll(p, &endp, 10);
    if (p == endp || errno != 0)
        return 0;
    *out = v;
    return 1;
}

static int json_get_double(const char *json, const char *key, double *out) {
    const char *p;
    char *endp;
    double v;

    if (!json_find_value_start(json, key, &p))
        return 0;
    errno = 0;
    v = strtod(p, &endp);
    if (p == endp || errno != 0)
        return 0;
    *out = v;
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out) {
    const char *p;
    if (!json_find_value_start(json, key, &p))
        return 0;
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

    if (!json_find_value_start(json, key, &p))
        return 0;
    if (*p != '"')
        return 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1])
            p++;
        if (n + 1 < out_sz)
            out[n++] = *p;
        p++;
    }
    if (*p != '"')
        return 0;
    if (out_sz > 0)
        out[n < out_sz ? n : out_sz - 1] = '\0';
    return 1;
}

static char *json_escape(const char *s) {
    size_t n = 0;
    const char *p;
    char *out;
    char *w;

    if (!s)
        return xstrdup("");

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
    if (!out)
        die("out of memory");

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
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        return 1;
    if (n > 1024)
        return 1024;
    return (int)n;
}

static int run_lfortran_rebuild_step(const cfg_t *cfg,
                                     FILE *fails,
                                     const char *build_dir,
                                     const char *missing_reason,
                                     const char *failed_reason) {
    cmd_result_t r = {0};
    char jobs_buf[32];
    char *cmd[8];
    int n = 0;

    if (!build_dir || !build_dir[0] || !dir_exists(build_dir)) {
        write_failure_row(fails,
                          "api_e2e",
                          "all",
                          "all",
                          "lfortran_llvm",
                          missing_reason,
                          2,
                          build_dir ? build_dir : "");
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
        write_failure_row(fails,
                          "api_e2e",
                          "all",
                          "all",
                          "lfortran_llvm",
                          failed_reason,
                          r.rc,
                          build_dir);
        return -1;
    }
    return 0;
}

static void usage(void) {
    printf("usage: bench_matrix [options]\n");
    printf("  --bench-dir PATH         output root (default: /tmp/liric_bench)\n");
    printf("  --build-dir PATH         build dir for benchmark binaries (default: build)\n");
    printf("  --manifest PATH          manifest path recorded in summary (default: tools/bench_manifest.json)\n");
    printf("  --modes LIST             comma list or 'all': isel,copy_patch,llvm\n");
    printf("  --policies LIST          comma list or 'all': direct,ir\n");
    printf("  --lanes LIST             comma list or 'all': ir_file,api_e2e,micro_c\n");
    printf("  --iters N                iterations forwarded to lane runners (default: 1)\n");
    printf("  --api-cases N            api_e2e cases per cell (default: 100, 0=all)\n");
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
}

static void set_all_modes(cfg_t *cfg, int v) {
    for (int i = 0; i < MODE_COUNT; i++)
        cfg->modes[i] = v;
}

static void set_all_policies(cfg_t *cfg, int v) {
    for (int i = 0; i < POLICY_COUNT; i++)
        cfg->policies[i] = v;
}

static void set_all_lanes(cfg_t *cfg, int v) {
    for (int i = 0; i < LANE_COUNT; i++)
        cfg->lanes[i] = v;
}

static int parse_policies(cfg_t *cfg, const char *text) {
    char *tmp = xstrdup(text);
    char *save = NULL;
    char *tok;

    set_all_policies(cfg, 0);
    tok = strtok_r(tmp, ",", &save);
    while (tok) {
        if (strcmp(tok, "direct") == 0)
            cfg->policies[POLICY_DIRECT] = 1;
        else if (strcmp(tok, "ir") == 0)
            cfg->policies[POLICY_IR] = 1;
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
        if (strcmp(tok, "ir_file") == 0)
            cfg->lanes[LANE_IR_FILE] = 1;
        else if (strcmp(tok, "api_e2e") == 0)
            cfg->lanes[LANE_API_E2E] = 1;
        else if (strcmp(tok, "micro_c") == 0)
            cfg->lanes[LANE_MICRO_C] = 1;
        else {
            free(tmp);
            return -1;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(tmp);
    return 0;
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    const char *default_lfortran_llvm = "../lfortran/build/src/bin/lfortran";
    const char *default_lfortran_liric_hyphen = "../lfortran/build-liric/src/bin/lfortran";
    const char *default_lfortran_liric_underscore = "../lfortran/build_liric/src/bin/lfortran";
    const char *default_lfortran_build_liric_hyphen = "../lfortran/build-liric";
    const char *default_lfortran_build_liric_underscore = "../lfortran/build_liric";
    const char *default_runtime_dylib =
        "../lfortran/build/src/runtime/liblfortran_runtime.dylib";
    const char *default_runtime_so =
        "../lfortran/build/src/runtime/liblfortran_runtime.so";

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
    cfg.bench_compat_check = NULL;
    cfg.bench_corpus_compare = NULL;
    cfg.bench_api = NULL;
    cfg.bench_tcc = NULL;
    cfg.probe_runner = NULL;
    cfg.lli_phases = NULL;
    cfg.cmake = "cmake";
    cfg.rebuild_lfortran = 1;
    cfg.lfortran = file_exists(default_lfortran_llvm) ? default_lfortran_llvm : NULL;
    cfg.lfortran_liric = file_exists(default_lfortran_liric_hyphen)
                             ? default_lfortran_liric_hyphen
                             : (file_exists(default_lfortran_liric_underscore)
                                    ? default_lfortran_liric_underscore
                                    : cfg.lfortran);
    cfg.lfortran_build_dir = "../lfortran/build";
    cfg.lfortran_liric_build_dir = dir_exists(default_lfortran_build_liric_hyphen)
                                       ? default_lfortran_build_liric_hyphen
                                       : (dir_exists(default_lfortran_build_liric_underscore)
                                              ? default_lfortran_build_liric_underscore
                                              : NULL);
    cfg.runtime_lib = file_exists(default_runtime_dylib)
                          ? default_runtime_dylib
                          : (file_exists(default_runtime_so)
                                 ? default_runtime_so
                                 : NULL);

    set_all_modes(&cfg, 1);
    set_all_policies(&cfg, 1);
    set_all_lanes(&cfg, 1);

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
            if (strcmp(v, "all") == 0)
                set_all_modes(&cfg, 1);
            else if (bench_parse_modes_csv(v, cfg.modes, MODE_COUNT) != 0)
                die("invalid --modes value: %s", v);
        } else if (strcmp(argv[i], "--policies") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "all") == 0)
                set_all_policies(&cfg, 1);
            else if (parse_policies(&cfg, v) != 0)
                die("invalid --policies value: %s", v);
        } else if (strcmp(argv[i], "--lanes") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "all") == 0)
                set_all_lanes(&cfg, 1);
            else if (parse_lanes(&cfg, v) != 0)
                die("invalid --lanes value: %s", v);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            cfg.iters = atoi(argv[++i]);
            if (cfg.iters <= 0)
                cfg.iters = 1;
        } else if (strcmp(argv[i], "--api-cases") == 0 && i + 1 < argc) {
            cfg.api_cases = atoi(argv[++i]);
            if (cfg.api_cases < 0)
                cfg.api_cases = 0;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0)
                cfg.timeout_sec = 15;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            cfg.timeout_ms = atoi(argv[++i]);
            if (cfg.timeout_ms <= 0)
                cfg.timeout_ms = 3000;
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
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }

    if (!cfg.bench_compat_check)
        cfg.bench_compat_check = bench_path_join2(cfg.build_dir, "bench_compat_check");
    if (!cfg.bench_corpus_compare)
        cfg.bench_corpus_compare = bench_path_join2(cfg.build_dir, "bench_lane_ir");
    if (!cfg.bench_api)
        cfg.bench_api = bench_path_join2(cfg.build_dir, "bench_lane_api");
    if (!cfg.bench_tcc)
        cfg.bench_tcc = bench_path_join2(cfg.build_dir, "bench_lane_micro");
    if (!cfg.probe_runner)
        cfg.probe_runner = bench_path_join2(cfg.build_dir, "liric_probe_runner");
    if (!cfg.lli_phases)
        cfg.lli_phases = bench_path_join2(cfg.build_dir, "bench_lli_phases");

    return cfg;
}

static int require_lane_selected(const cfg_t *cfg) {
    for (int i = 0; i < LANE_COUNT; i++) {
        if (cfg->lanes[i])
            return 1;
    }
    return 0;
}

static int require_mode_selected(const cfg_t *cfg) {
    for (int i = 0; i < MODE_COUNT; i++) {
        if (cfg->modes[i])
            return 1;
    }
    return 0;
}

static int require_policy_selected(const cfg_t *cfg) {
    for (int i = 0; i < POLICY_COUNT; i++) {
        if (cfg->policies[i])
            return 1;
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
            e_lane,
            e_mode,
            e_policy,
            e_base,
            e_reason,
            rc,
            e_summary);

    free(e_lane);
    free(e_mode);
    free(e_policy);
    free(e_base);
    free(e_reason);
    free(e_summary);
}

static void write_row_ir(FILE *rf,
                         const char *mode,
                         const char *policy,
                         const char *summary_path,
                         const char *status,
                         long long attempted,
                         long long completed,
                         double sp_nonparse,
                         double sp_total) {
    char *e_summary = json_escape(summary_path);
    fprintf(rf,
            "{\"lane\":\"ir_file\",\"track\":\"corpus_canonical\","
            "\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"llvm\",\"status\":\"%s\","
            "\"attempted\":%lld,\"completed\":%lld,"
            "\"speedup_nonparse_median\":%.6f,\"speedup_total_median\":%.6f,"
            "\"summary\":\"%s\"}\n",
            mode,
            policy,
            status,
            attempted,
            completed,
            sp_nonparse,
            sp_total,
            e_summary);
    free(e_summary);
}

static void write_row_api(FILE *rf,
                          const char *mode,
                          const char *policy,
                          const char *summary_path,
                          const char *status,
                          long long attempted,
                          long long completed,
                          long long skipped,
                          int zero_skip_met) {
    char *e_summary = json_escape(summary_path);
    fprintf(rf,
            "{\"lane\":\"api_e2e\",\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"lfortran_llvm\","
            "\"status\":\"%s\",\"attempted\":%lld,\"completed\":%lld,\"skipped\":%lld,"
            "\"zero_skip_gate_met\":%s,\"summary\":\"%s\"}\n",
            mode,
            policy,
            status,
            attempted,
            completed,
            skipped,
            zero_skip_met ? "true" : "false",
            e_summary);
    free(e_summary);
}

static void write_row_tcc(FILE *rf,
                          const char *mode,
                          const char *policy,
                          const char *summary_path,
                          const char *status,
                          long long total_cases,
                          long long wall_passed,
                          long long inproc_passed,
                          double wall_ratio,
                          double inproc_ratio) {
    char *e_summary = json_escape(summary_path);
    fprintf(rf,
            "{\"lane\":\"micro_c\",\"mode\":\"%s\",\"policy\":\"%s\",\"baseline\":\"tcc\","
            "\"status\":\"%s\",\"total_cases\":%lld,\"wall_passed\":%lld,"
            "\"inproc_passed\":%lld,\"speedup_wall_total\":%.6f,"
            "\"speedup_nonparse_total\":%.6f,\"summary\":\"%s\"}\n",
            mode,
            policy,
            status,
            total_cases,
            wall_passed,
            inproc_passed,
            wall_ratio,
            inproc_ratio,
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

static void write_row_compat(FILE *rf,
                             const char *status,
                             long long compat_api_n,
                             long long compat_ll_n,
                             const char *bench_dir) {
    char *e_bench_dir = json_escape(bench_dir ? bench_dir : "");
    fprintf(rf,
            "{\"lane\":\"compat_check\",\"mode\":\"all\",\"policy\":\"all\",\"baseline\":\"lfortran_llvm\","
            "\"status\":\"%s\",\"compat_api_count\":%lld,\"compat_ll_count\":%lld,"
            "\"summary\":\"%s\"}\n",
            status,
            compat_api_n,
            compat_ll_n,
            e_bench_dir);
    free(e_bench_dir);
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    FILE *rows = NULL;
    FILE *fails = NULL;
    char *rows_path = NULL;
    char *fails_path = NULL;
    char *summary_path = NULL;
    char *compat_ll = NULL;
    char *compat_api = NULL;
    char *compat_opts = NULL;

    int cells_attempted = 0;
    int cells_ok = 0;
    int cells_failed = 0;
    int compat_ok = 1;
    int ran_compat = 0;

    if (!require_lane_selected(&cfg))
        die("no lanes selected");
    if (!require_mode_selected(&cfg))
        die("no modes selected");
    if (!require_policy_selected(&cfg))
        die("no policies selected");

    if (cfg.manifest && !file_exists(cfg.manifest))
        die("manifest missing: %s", cfg.manifest);

    if (mkdir_p(cfg.bench_dir) != 0)
        die("failed to create bench dir: %s", cfg.bench_dir);

    rows_path = bench_path_join2(cfg.bench_dir, "matrix_rows.jsonl");
    fails_path = bench_path_join2(cfg.bench_dir, "matrix_failures.jsonl");
    summary_path = bench_path_join2(cfg.bench_dir, "matrix_summary.json");
    compat_ll = bench_path_join2(cfg.bench_dir, "compat_ll.txt");
    compat_api = bench_path_join2(cfg.bench_dir, "compat_api.txt");
    compat_opts = bench_path_join2(cfg.bench_dir, "compat_ll_options.jsonl");

    rows = fopen(rows_path, "w");
    if (!rows)
        die("failed to open rows output: %s", rows_path);
    fails = fopen(fails_path, "w");
    if (!fails)
        die("failed to open failures output: %s", fails_path);

    if (cfg.lanes[LANE_API_E2E] && cfg.rebuild_lfortran) {
        if (run_lfortran_rebuild_step(&cfg,
                                      fails,
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
                              "api_e2e",
                              "all",
                              "all",
                              "lfortran_llvm",
                              "lfortran_liric_build_dir_missing",
                              2,
                              "");
            compat_ok = 0;
        }
    }

    if (cfg.lanes[LANE_API_E2E] && cfg.run_compat_check) {
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
                              "api_e2e",
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
                                  "api_e2e",
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
                                  "api_e2e",
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

    for (int mi = 0; mi < MODE_COUNT; mi++) {
        const char *mode;
        if (!cfg.modes[mi])
            continue;
        mode = k_mode_name[mi];

        for (int pi = 0; pi < POLICY_COUNT; pi++) {
            const char *policy;
            if (!cfg.policies[pi])
                continue;
            policy = k_policy_name[pi];

            for (int li = 0; li < LANE_COUNT; li++) {
                const char *lane;
                char *mode_dir = NULL;
                char *policy_dir = NULL;
                char *lane_dir = NULL;

                if (!cfg.lanes[li])
                    continue;
                lane = k_lane_name[li];

                mode_dir = bench_path_join2(cfg.bench_dir, mode);
                policy_dir = bench_path_join2(mode_dir, policy);
                lane_dir = bench_path_join2(policy_dir, lane);
                if (mkdir_p(lane_dir) != 0)
                    die("failed to create lane dir: %s", lane_dir);

                cells_attempted++;
                printf("[matrix] mode=%s policy=%s lane=%s\n", mode, policy, lane);

                if (li == LANE_IR_FILE) {
                    cmd_result_t r = {0};
                    char iters_buf[32], timeout_buf[32];
                    char *sum_path = bench_path_join2(lane_dir, "bench_corpus_compare_summary.json");
                    char *cmd[44];
                    int n = 0;
                    char status[64] = "UNKNOWN";
                    long long attempted = 0, completed = 0;
                    double sp_nonparse = 0.0, sp_total = 0.0;
                    int ok = 0;

                    if (!file_executable(cfg.bench_corpus_compare)) {
                        write_failure_row(fails, lane, mode, policy, "llvm", "bench_corpus_compare_missing", 127, cfg.bench_corpus_compare);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }
                    if (!file_executable(cfg.probe_runner)) {
                        write_failure_row(fails, lane, mode, policy, "llvm", "liric_probe_runner_missing", 127, cfg.probe_runner);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }
                    if (!file_executable(cfg.lli_phases)) {
                        write_failure_row(fails, lane, mode, policy, "llvm", "bench_lli_phases_missing", 127, cfg.lli_phases);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    snprintf(iters_buf, sizeof(iters_buf), "%d", cfg.iters);
                    snprintf(timeout_buf, sizeof(timeout_buf), "%d", cfg.timeout_sec);

                    cmd[n++] = (char *)cfg.bench_corpus_compare;
                    cmd[n++] = "--bench-dir";
                    cmd[n++] = lane_dir;
                    cmd[n++] = "--iters";
                    cmd[n++] = iters_buf;
                    cmd[n++] = "--timeout";
                    cmd[n++] = timeout_buf;
                    cmd[n++] = "--policy";
                    cmd[n++] = (char *)policy;
                    cmd[n++] = "--probe-runner";
                    cmd[n++] = (char *)cfg.probe_runner;
                    cmd[n++] = "--lli-phases";
                    cmd[n++] = (char *)cfg.lli_phases;
                    if (cfg.runtime_lib) {
                        cmd[n++] = "--runtime-lib";
                        cmd[n++] = (char *)cfg.runtime_lib;
                    }
                    if (cfg.corpus) {
                        cmd[n++] = "--corpus";
                        cmd[n++] = (char *)cfg.corpus;
                    }
                    if (cfg.cache_dir) {
                        cmd[n++] = "--cache-dir";
                        cmd[n++] = (char *)cfg.cache_dir;
                    }
                    cmd[n++] = NULL;

                    if (run_cmd_with_mode(mode, cmd, &r) != 0 || r.rc != 0) {
                        write_failure_row(fails, lane, mode, policy, "llvm", "bench_corpus_compare_failed", r.rc, sum_path);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    {
                        char *json = bench_read_all_file(sum_path);
                        if (!json) {
                            write_failure_row(fails, lane, mode, policy, "llvm", "summary_missing", 1, sum_path);
                            cells_failed++;
                            free(sum_path);
                            free(mode_dir);
                            free(policy_dir);
                            free(lane_dir);
                            continue;
                        }

                        (void)json_get_string(json, "status", status, sizeof(status));
                        (void)json_get_int64(json, "attempted_tests", &attempted);
                        (void)json_get_int64(json, "completed_tests", &completed);
                        (void)json_get_double(json, "compile_materialized_speedup_median", &sp_nonparse);
                        (void)json_get_double(json, "total_materialized_speedup_median", &sp_total);

                        ok = (strcmp(status, "OK") == 0 && attempted > 0 && completed == attempted);
                        write_row_ir(rows, mode, policy, sum_path, status, attempted, completed, sp_nonparse, sp_total);
                        free(json);
                    }

                    if (ok)
                        cells_ok++;
                    else {
                        write_failure_row(fails, lane, mode, policy, "llvm", "ir_lane_incomplete", 1, sum_path);
                        cells_failed++;
                    }

                    free(sum_path);
                } else if (li == LANE_API_E2E) {
                    cmd_result_t r = {0};
                    char iters_buf[32], timeout_buf[32], min_completed_buf[32], api_cases_buf[32];
                    char *sum_path = bench_path_join2(lane_dir, "bench_api_summary.json");
                    char *cmd[56];
                    int n = 0;
                    char status[64] = "UNKNOWN";
                    long long attempted = 0, completed = 0, skipped = 0;
                    int zero_skip = 0;
                    int ok = 0;

                    if (!compat_ok) {
                        write_failure_row(fails, lane, mode, policy, "lfortran_llvm", "compat_check_unavailable", 1, cfg.bench_dir);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    if (!file_executable(cfg.bench_api)) {
                        write_failure_row(fails, lane, mode, policy, "lfortran_llvm", "bench_api_missing", 127, cfg.bench_api);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    snprintf(iters_buf, sizeof(iters_buf), "%d", cfg.iters);
                    snprintf(timeout_buf, sizeof(timeout_buf), "%d", cfg.timeout_ms);
                    snprintf(min_completed_buf, sizeof(min_completed_buf), "%d", 1);
                    snprintf(api_cases_buf, sizeof(api_cases_buf), "%d", cfg.api_cases);

                    cmd[n++] = (char *)cfg.bench_api;
                    cmd[n++] = "--bench-dir";
                    cmd[n++] = lane_dir;
                    cmd[n++] = "--iters";
                    cmd[n++] = iters_buf;
                    cmd[n++] = "--timeout-ms";
                    cmd[n++] = timeout_buf;
                    cmd[n++] = "--min-completed";
                    cmd[n++] = min_completed_buf;
                    cmd[n++] = "--require-zero-skips";
                    cmd[n++] = "--liric-policy";
                    cmd[n++] = (char *)policy;
                    cmd[n++] = "--compat-list";
                    cmd[n++] = compat_ll;
                    cmd[n++] = "--options-jsonl";
                    cmd[n++] = compat_opts;
                    if (cfg.api_cases > 0) {
                        cmd[n++] = "--fail-sample-limit";
                        cmd[n++] = api_cases_buf;
                    }
                    if (cfg.lfortran) {
                        cmd[n++] = "--lfortran";
                        cmd[n++] = (char *)cfg.lfortran;
                    }
                    if (cfg.lfortran_liric) {
                        cmd[n++] = "--lfortran-liric";
                        cmd[n++] = (char *)cfg.lfortran_liric;
                    }
                    if (cfg.test_dir) {
                        cmd[n++] = "--test-dir";
                        cmd[n++] = (char *)cfg.test_dir;
                    }
                    if (cfg.runtime_lib) {
                        cmd[n++] = "--runtime-lib";
                        cmd[n++] = (char *)cfg.runtime_lib;
                    }
                    cmd[n++] = NULL;

                    if (run_cmd_with_mode(mode, cmd, &r) != 0 || r.rc != 0) {
                        write_failure_row(fails, lane, mode, policy, "lfortran_llvm", "bench_api_failed", r.rc, sum_path);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    {
                        char *json = bench_read_all_file(sum_path);
                        if (!json) {
                            write_failure_row(fails, lane, mode, policy, "lfortran_llvm", "summary_missing", 1, sum_path);
                            cells_failed++;
                            free(sum_path);
                            free(mode_dir);
                            free(policy_dir);
                            free(lane_dir);
                            continue;
                        }

                        (void)json_get_string(json, "status", status, sizeof(status));
                        (void)json_get_int64(json, "attempted", &attempted);
                        (void)json_get_int64(json, "completed", &completed);
                        (void)json_get_int64(json, "skipped", &skipped);
                        (void)json_get_bool(json, "zero_skip_gate_met", &zero_skip);

                        ok = (strcmp(status, "OK") == 0 && attempted > 0 && completed == attempted && skipped == 0 && zero_skip);
                        write_row_api(rows, mode, policy, sum_path, status, attempted, completed, skipped, zero_skip);
                        free(json);
                    }

                    if (ok)
                        cells_ok++;
                    else {
                        write_failure_row(fails, lane, mode, policy, "lfortran_llvm", "api_lane_incomplete", 1, sum_path);
                        cells_failed++;
                    }

                    free(sum_path);
                } else if (li == LANE_MICRO_C) {
                    cmd_result_t r = {0};
                    char iters_buf[32];
                    char *sum_path = bench_path_join2(lane_dir, "bench_tcc_summary.json");
                    char *cmd[20];
                    int n = 0;
                    char status[64] = "UNKNOWN";
                    long long total_cases = 0, wall_passed = 0, inproc_passed = 0;
                    double wall_ratio = 0.0, inproc_ratio = 0.0;
                    int ok = 0;

                    if (!file_executable(cfg.bench_tcc)) {
                        write_failure_row(fails, lane, mode, policy, "tcc", "bench_tcc_missing", 127, cfg.bench_tcc);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    snprintf(iters_buf, sizeof(iters_buf), "%d", cfg.iters);
                    cmd[n++] = (char *)cfg.bench_tcc;
                    cmd[n++] = "--iters";
                    cmd[n++] = iters_buf;
                    cmd[n++] = "--policy";
                    cmd[n++] = (char *)policy;
                    cmd[n++] = "--bench-dir";
                    cmd[n++] = lane_dir;
                    cmd[n++] = NULL;

                    if (run_cmd_with_mode(mode, cmd, &r) != 0 || r.rc != 0) {
                        write_failure_row(fails, lane, mode, policy, "tcc", "bench_tcc_failed", r.rc, sum_path);
                        cells_failed++;
                        free(sum_path);
                        free(mode_dir);
                        free(policy_dir);
                        free(lane_dir);
                        continue;
                    }

                    {
                        char *json = bench_read_all_file(sum_path);
                        if (!json) {
                            write_failure_row(fails, lane, mode, policy, "tcc", "summary_missing", 1, sum_path);
                            cells_failed++;
                            free(sum_path);
                            free(mode_dir);
                            free(policy_dir);
                            free(lane_dir);
                            continue;
                        }

                        (void)json_get_string(json, "status", status, sizeof(status));
                        (void)json_get_int64(json, "total_cases", &total_cases);
                        (void)json_get_int64(json, "wall_passed", &wall_passed);
                        (void)json_get_int64(json, "inproc_passed", &inproc_passed);
                        (void)json_get_double(json, "wall_speedup_ratio", &wall_ratio);
                        (void)json_get_double(json, "inproc_speedup_ratio", &inproc_ratio);

                        ok = (strcmp(status, "OK") == 0 && total_cases > 0 && wall_passed == total_cases && inproc_passed == total_cases);
                        write_row_tcc(rows, mode, policy, sum_path, status, total_cases, wall_passed, inproc_passed, wall_ratio, inproc_ratio);
                        free(json);
                    }

                    if (ok)
                        cells_ok++;
                    else {
                        write_failure_row(fails, lane, mode, policy, "tcc", "micro_lane_incomplete", 1, sum_path);
                        cells_failed++;
                    }

                    free(sum_path);
                }

                free(mode_dir);
                free(policy_dir);
                free(lane_dir);
            }
        }
    }

    fclose(rows);
    fclose(fails);

    {
        FILE *sf = fopen(summary_path, "w");
        char ts[64] = {0};
        char status[16];
        if (!sf)
            die("failed to write summary: %s", summary_path);

        format_iso8601_utc(ts, sizeof(ts));
        strcpy(status, (cells_attempted > 0 && cells_failed == 0) ? "OK" : "FAILED");

        fprintf(sf, "{\n");
        fprintf(sf, "  \"schema_version\": 1,\n");
        fprintf(sf, "  \"generated_at_utc\": \"%s\",\n", ts);
        {
            char *e_bench_dir = json_escape(cfg.bench_dir);
            char *e_manifest = json_escape(cfg.manifest ? cfg.manifest : "");
            char *e_rows = json_escape(rows_path);
            char *e_fails = json_escape(fails_path);
            fprintf(sf, "  \"bench_dir\": \"%s\",\n", e_bench_dir);
            fprintf(sf, "  \"manifest\": \"%s\",\n", e_manifest);
            fprintf(sf, "  \"rows_jsonl\": \"%s\",\n", e_rows);
            fprintf(sf, "  \"failures_jsonl\": \"%s\",\n", e_fails);
            free(e_bench_dir);
            free(e_manifest);
            free(e_rows);
            free(e_fails);
        }
        fprintf(sf, "  \"status\": \"%s\",\n", status);
        fprintf(sf, "  \"cells_attempted\": %d,\n", cells_attempted);
        fprintf(sf, "  \"cells_ok\": %d,\n", cells_ok);
        fprintf(sf, "  \"cells_failed\": %d,\n", cells_failed);
        fprintf(sf, "  \"ran_compat_check\": %s,\n", ran_compat ? "true" : "false");
        fprintf(sf, "  \"compat_ok\": %s\n", compat_ok ? "true" : "false");
        fprintf(sf, "}\n");
        fclose(sf);
    }

    printf("[matrix] summary: %s\n", summary_path);
    printf("[matrix] rows:    %s\n", rows_path);
    printf("[matrix] fails:   %s\n", fails_path);
    printf("[matrix] cells: attempted=%d ok=%d failed=%d\n", cells_attempted, cells_ok, cells_failed);

    if (cells_attempted == 0) {
        fprintf(stderr, "no matrix cells attempted\n");
        free(rows_path);
        free(fails_path);
        free(summary_path);
        free(compat_ll);
        free(compat_api);
        free(compat_opts);
        return 1;
    }

    free(rows_path);
    free(fails_path);
    free(summary_path);
    free(compat_ll);
    free(compat_api);
    free(compat_opts);

    if (cells_failed > 0 && !cfg.allow_partial)
        return 1;
    return 0;
}
