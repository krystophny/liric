// Corpus benchmark comparator: liric_probe_runner vs bench_lli_phases on corpus_100.
// Publishes one canonical JSONL and one summary JSON.

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bench_common.h"

typedef struct {
    char *name;
    char *ll_path;
} test_case_t;

typedef struct {
    test_case_t *items;
    size_t n;
    size_t cap;
} testlist_t;

typedef bench_cmd_result_t cmd_result_t;

typedef struct {
    char *name;
    double liric_parse_ms;
    double liric_compile_ms;
    double liric_lookup_ms;
    double liric_compile_materialized_ms;
    double liric_total_materialized_ms;

    double llvm_parse_ms;
    double llvm_parse_input_ms;
    double llvm_add_module_ms;
    double llvm_lookup_ms;
    double llvm_compile_materialized_ms;
    double llvm_total_materialized_ms;

    double compile_materialized_speedup;
    double total_materialized_speedup;
} row_t;

typedef struct {
    row_t *items;
    size_t n;
    size_t cap;
} rowlist_t;

typedef struct {
    const char *probe_runner;
    const char *lli_phases;
    const char *runtime_lib;
    const char *policy;
    const char *corpus_tsv;
    const char *cache_dir;
    const char *bench_dir;
    int timeout_sec;
    int allow_empty;
} cfg_t;

typedef struct {
    size_t attempted;
    size_t completed;

    double liric_parse_median_ms;
    double liric_compile_median_ms;
    double liric_lookup_median_ms;
    double liric_compile_materialized_median_ms;
    double liric_total_materialized_median_ms;

    double llvm_parse_median_ms;
    double llvm_parse_input_median_ms;
    double llvm_add_module_median_ms;
    double llvm_lookup_median_ms;
    double llvm_compile_materialized_median_ms;
    double llvm_total_materialized_median_ms;

    double compile_materialized_speedup_median;
    double total_materialized_speedup_median;
    double compile_materialized_speedup_aggregate;
    double total_materialized_speedup_aggregate;

    double liric_total_materialized_aggregate_ms;
    double llvm_total_materialized_aggregate_ms;
} summary_t;

typedef struct {
    double read_ms;
    double parse_ms;
    double jit_create_ms;
    double load_lib_ms;
    double compile_ms;
    double lookup_ms;
    double exec_ms;
    double total_ms;
} probe_timing_t;

static void die(const char *msg, const char *path) {
    if (path) fprintf(stderr, "%s: %s\n", msg, path);
    else fprintf(stderr, "%s\n", msg);
    exit(1);
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

static char *xstrdup(const char *s) {
    char *p;
    p = bench_xstrdup(s);
    if (!p && s) die("out of memory", NULL);
    return p;
}

static char *to_abs_path(const char *path) {
    char *out;
    out = bench_to_abs_path(path);
    if (!out && path) die("failed to resolve absolute path", path);
    return out;
}

static char *path_join2(const char *a, const char *b) {
    char *out = bench_path_join2(a, b);
    if (!out) die("out of memory", NULL);
    return out;
}

static cmd_result_t run_cmd(char *const argv[], int timeout_sec, const char *work_dir) {
    bench_run_cmd_opts_t opts;
    cmd_result_t r;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = timeout_sec > 0 ? timeout_sec * 1000 : 0;
    opts.work_dir = work_dir;
    if (bench_run_cmd(&opts, &r) != 0) die("failed to run command", argv[0]);
    return r;
}

static void free_cmd_result(cmd_result_t *r) {
    bench_free_cmd_result(r);
}

static double median(const double *vals, size_t n) {
    return bench_median(vals, n);
}

static int parse_probe_timing(const char *stderr_text,
                              probe_timing_t *out_timing) {
    const char *p = strstr(stderr_text, "TIMING ");
    double read_us, parse_us, jit_create_us, load_lib_us, compile_us, lookup_us, exec_us, total_us;
    double run_us;
    if (!p) return 0;
    if (sscanf(p,
               "TIMING read_us=%lf parse_us=%lf jit_create_us=%lf load_lib_us=%lf compile_us=%lf lookup_us=%lf exec_us=%lf total_us=%lf",
               &read_us, &parse_us, &jit_create_us, &load_lib_us, &compile_us, &lookup_us, &exec_us, &total_us) == 8) {
        out_timing->read_ms = read_us / 1000.0;
        out_timing->parse_ms = parse_us / 1000.0;
        out_timing->jit_create_ms = jit_create_us / 1000.0;
        out_timing->load_lib_ms = load_lib_us / 1000.0;
        out_timing->compile_ms = compile_us / 1000.0;
        out_timing->lookup_ms = lookup_us / 1000.0;
        out_timing->exec_ms = exec_us / 1000.0;
        out_timing->total_ms = total_us / 1000.0;
        return 1;
    }
    if (sscanf(p,
               "TIMING read_us=%lf parse_us=%lf jit_create_us=%lf load_lib_us=%lf compile_us=%lf run_us=%lf total_us=%lf",
               &read_us, &parse_us, &jit_create_us, &load_lib_us, &compile_us, &run_us, &total_us) == 7) {
        out_timing->read_ms = read_us / 1000.0;
        out_timing->parse_ms = parse_us / 1000.0;
        out_timing->jit_create_ms = jit_create_us / 1000.0;
        out_timing->load_lib_ms = load_lib_us / 1000.0;
        out_timing->compile_ms = compile_us / 1000.0;
        out_timing->lookup_ms = 0.0;
        out_timing->exec_ms = run_us / 1000.0;
        out_timing->total_ms = total_us / 1000.0;
        return 1;
    }
    return 0;
}

static int ll_has_defined_main(const char *ll_path) {
    FILE *f;
    char line[4096];
    int has_main = 0;
    f = fopen(ll_path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "define", 6) != 0 ||
            !(p[6] == ' ' || p[6] == '\t'))
            continue;
        if (strstr(p, "@main(") || strstr(p, "@\"main\"(")) {
            has_main = 1;
            break;
        }
    }
    fclose(f);
    return has_main;
}

static int json_get_number(const char *json, const char *key, double *out_val) {
    return bench_json_get_number(json, key, out_val);
}

static int is_valid_policy(const char *policy) {
    return policy && (strcmp(policy, "direct") == 0 || strcmp(policy, "ir") == 0);
}

static void testlist_init(testlist_t *l) {
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

static void testlist_push(testlist_t *l, const char *name, const char *ll_path) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 128;
        test_case_t *nitems = (test_case_t *)realloc(l->items, ncap * sizeof(test_case_t));
        if (!nitems) die("out of memory", NULL);
        l->items = nitems;
        l->cap = ncap;
    }
    l->items[l->n].name = xstrdup(name);
    l->items[l->n].ll_path = xstrdup(ll_path);
    l->n++;
}

static void testlist_free(testlist_t *l) {
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->items[i].name);
        free(l->items[i].ll_path);
    }
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static void rowlist_init(rowlist_t *l) {
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

static void rowlist_push(rowlist_t *l, row_t row) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 128;
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

static int load_corpus_tests(const cfg_t *cfg, testlist_t *tests) {
    FILE *f = fopen(cfg->corpus_tsv, "r");
    char line[2048];
    if (!f) {
        fprintf(stderr, "cannot open corpus TSV: %s\n", cfg->corpus_tsv);
        return 0;
    }

    while (fgets(line, sizeof(line), f)) {
        char *case_id;
        char *name;
        char *tab1;
        char *tab2;
        char *ll_path;

        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) continue;

        tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = '\0';
        tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab2 = '\0';

        case_id = line;
        name = tab1 + 1;

        ll_path = path_join2(cfg->cache_dir, case_id);
        {
            char *tmp = path_join2(ll_path, "raw.ll");
            free(ll_path);
            ll_path = tmp;
        }
        if (ll_path && file_exists(ll_path) && ll_has_defined_main(ll_path))
            testlist_push(tests, name, ll_path);
        free(ll_path);
    }

    fclose(f);
    return (int)tests->n;
}

static void usage(void) {
    printf("usage: bench_corpus_compare [options]\n");
    printf("  --timeout N           command timeout in seconds (default: 30)\n");
    printf("  --probe-runner PATH   path to liric_probe_runner\n");
    printf("  --lli-phases PATH     path to bench_lli_phases\n");
    printf("  --policy MODE         liric session policy: direct|ir (default: direct)\n");
    printf("  --runtime-lib PATH    runtime shared library (required for runtime-dependent cases)\n");
    printf("  --corpus PATH         corpus TSV (default: tools/corpus_100.tsv)\n");
    printf("  --cache-dir PATH      corpus cache dir (default: /tmp/liric_lfortran_mass/cache)\n");
    printf("  --bench-dir PATH      benchmark output dir (default: /tmp/liric_bench)\n");
    printf("  --allow-empty         allow empty dataset\n");
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    const char *default_runtime_dylib =
        "build/deps/lfortran/build-llvm/src/runtime/liblfortran_runtime.dylib";
    const char *default_runtime_so =
        "build/deps/lfortran/build-llvm/src/runtime/liblfortran_runtime.so";

    cfg.probe_runner = "build/liric_probe_runner";
    cfg.lli_phases = "build/bench_lli_phases";
    cfg.policy = "direct";
    cfg.runtime_lib = file_exists(default_runtime_dylib)
                          ? default_runtime_dylib
                          : (file_exists(default_runtime_so)
                                 ? default_runtime_so
                                 : NULL);
    cfg.corpus_tsv = "tools/corpus_100.tsv";
    cfg.cache_dir = "/tmp/liric_lfortran_mass/cache";
    cfg.bench_dir = "/tmp/liric_bench";
    cfg.timeout_sec = 30;
    cfg.allow_empty = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0) cfg.timeout_sec = 30;
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg.probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--lli-phases") == 0 && i + 1 < argc) {
            cfg.lli_phases = argv[++i];
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            cfg.runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            cfg.policy = argv[++i];
        } else if (strcmp(argv[i], "--corpus") == 0 && i + 1 < argc) {
            cfg.corpus_tsv = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cfg.cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            cfg.bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--allow-empty") == 0) {
            cfg.allow_empty = 1;
        } else {
            die("unknown argument", argv[i]);
        }
    }

    if (!file_exists(cfg.probe_runner)) die("probe runner not found", cfg.probe_runner);
    if (!file_exists(cfg.lli_phases)) die("bench_lli_phases not found", cfg.lli_phases);
    if (!file_exists(cfg.corpus_tsv)) die("corpus TSV not found", cfg.corpus_tsv);
    if (!is_valid_policy(cfg.policy)) die("invalid --policy (expected direct|ir)", cfg.policy);

    cfg.probe_runner = to_abs_path(cfg.probe_runner);
    cfg.lli_phases = to_abs_path(cfg.lli_phases);
    if (cfg.runtime_lib && cfg.runtime_lib[0]) {
        if (!file_exists(cfg.runtime_lib))
            die("runtime library not found", cfg.runtime_lib);
        cfg.runtime_lib = to_abs_path(cfg.runtime_lib);
    }
    cfg.corpus_tsv = to_abs_path(cfg.corpus_tsv);
    cfg.cache_dir = to_abs_path(cfg.cache_dir);
    cfg.bench_dir = to_abs_path(cfg.bench_dir);

    return cfg;
}

static void summarize_rows(const rowlist_t *rows, summary_t *s) {
    size_t i;
    memset(s, 0, sizeof(*s));
    s->completed = rows->n;
    if (rows->n == 0)
        return;

    {
        size_t n = rows->n;
        double *lp = (double *)malloc(n * sizeof(double));
        double *lc = (double *)malloc(n * sizeof(double));
        double *ll = (double *)malloc(n * sizeof(double));
        double *lcm = (double *)malloc(n * sizeof(double));
        double *ltm = (double *)malloc(n * sizeof(double));
        double *ep = (double *)malloc(n * sizeof(double));
        double *epi = (double *)malloc(n * sizeof(double));
        double *eam = (double *)malloc(n * sizeof(double));
        double *el = (double *)malloc(n * sizeof(double));
        double *ecm = (double *)malloc(n * sizeof(double));
        double *etm = (double *)malloc(n * sizeof(double));
        double *spc = (double *)malloc(n * sizeof(double));
        double *spt = (double *)malloc(n * sizeof(double));

        if (!lp || !lc || !ll || !lcm || !ltm || !ep || !epi || !eam ||
            !el || !ecm || !etm || !spc || !spt) {
            die("out of memory", NULL);
        }

        for (i = 0; i < n; i++) {
            const row_t *r = &rows->items[i];
            lp[i] = r->liric_parse_ms;
            lc[i] = r->liric_compile_ms;
            ll[i] = r->liric_lookup_ms;
            lcm[i] = r->liric_compile_materialized_ms;
            ltm[i] = r->liric_total_materialized_ms;
            ep[i] = r->llvm_parse_ms;
            epi[i] = r->llvm_parse_input_ms;
            eam[i] = r->llvm_add_module_ms;
            el[i] = r->llvm_lookup_ms;
            ecm[i] = r->llvm_compile_materialized_ms;
            etm[i] = r->llvm_total_materialized_ms;
            spc[i] = r->compile_materialized_speedup;
            spt[i] = r->total_materialized_speedup;
            s->liric_total_materialized_aggregate_ms += r->liric_total_materialized_ms;
            s->llvm_total_materialized_aggregate_ms += r->llvm_total_materialized_ms;
        }

        s->liric_parse_median_ms = median(lp, n);
        s->liric_compile_median_ms = median(lc, n);
        s->liric_lookup_median_ms = median(ll, n);
        s->liric_compile_materialized_median_ms = median(lcm, n);
        s->liric_total_materialized_median_ms = median(ltm, n);
        s->llvm_parse_median_ms = median(ep, n);
        s->llvm_parse_input_median_ms = median(epi, n);
        s->llvm_add_module_median_ms = median(eam, n);
        s->llvm_lookup_median_ms = median(el, n);
        s->llvm_compile_materialized_median_ms = median(ecm, n);
        s->llvm_total_materialized_median_ms = median(etm, n);
        s->compile_materialized_speedup_median = median(spc, n);
        s->total_materialized_speedup_median = median(spt, n);

        if (s->liric_total_materialized_aggregate_ms > 0.0) {
            s->total_materialized_speedup_aggregate =
                s->llvm_total_materialized_aggregate_ms / s->liric_total_materialized_aggregate_ms;
        }
        {
            double liric_compile_agg = 0.0;
            double llvm_compile_agg = 0.0;
            for (i = 0; i < n; i++) {
                liric_compile_agg += lcm[i];
                llvm_compile_agg += ecm[i];
            }
            if (liric_compile_agg > 0.0)
                s->compile_materialized_speedup_aggregate = llvm_compile_agg / liric_compile_agg;
        }

        free(lp);
        free(lc);
        free(ll);
        free(lcm);
        free(ltm);
        free(ep);
        free(epi);
        free(eam);
        free(el);
        free(ecm);
        free(etm);
        free(spc);
        free(spt);
    }
}

static int run_suite(const cfg_t *cfg,
                     const testlist_t *tests,
                     const char *jsonl_path,
                     summary_t *out_summary) {
    FILE *jf;
    size_t i;
    rowlist_t rows;

    out_summary->attempted = tests->n;
    out_summary->completed = 0;

    rowlist_init(&rows);
    jf = fopen(jsonl_path, "w");
    if (!jf) die("failed to open output", jsonl_path);

    printf("Corpus compare: %zu tests\n", tests->n);

    for (i = 0; i < tests->n; i++) {
        const test_case_t *t = &tests->items[i];
        size_t ok_n = 0;
        int skipped = 0;
        size_t it;

        double *liric_parse = (double *)calloc(1, sizeof(double));
        double *liric_compile = (double *)calloc(1, sizeof(double));
        double *liric_lookup = (double *)calloc(1, sizeof(double));

        double *llvm_parse = (double *)calloc(1, sizeof(double));
        double *llvm_parse_input = (double *)calloc(1, sizeof(double));
        double *llvm_add_module = (double *)calloc(1, sizeof(double));
        double *llvm_lookup = (double *)calloc(1, sizeof(double));
        double *llvm_compile_mat = (double *)calloc(1, sizeof(double));

        if (!liric_parse || !liric_compile || !liric_lookup || !llvm_parse ||
            !llvm_parse_input || !llvm_add_module || !llvm_lookup || !llvm_compile_mat) {
            die("out of memory", NULL);
        }

        for (it = 0; it < 1; it++) {
            cmd_result_t rp, ri;
            double l_parse = 0.0, l_compile = 0.0, l_lookup = 0.0;
            double e_parse = 0.0, e_parse_input = 0.0;
            double e_add = 0.0, e_lookup = 0.0, e_compile_mat = 0.0;
            probe_timing_t tp_full;
            memset(&tp_full, 0, sizeof(tp_full));

            char *probe_argv[20];
            int pk = 0;

            /* Compatibility path for probe runner binaries without --no-exec/--parse-only. */
            probe_argv[pk++] = (char *)cfg->probe_runner;
            probe_argv[pk++] = "--timing";
            probe_argv[pk++] = "--no-exec";
            probe_argv[pk++] = "--policy";
            probe_argv[pk++] = (char *)cfg->policy;
            probe_argv[pk++] = "--func";
            probe_argv[pk++] = "main";
            probe_argv[pk++] = "--sig";
            probe_argv[pk++] = "i32_argc_argv";
            if (cfg->runtime_lib && cfg->runtime_lib[0]) {
                probe_argv[pk++] = "--load-lib";
                probe_argv[pk++] = (char *)cfg->runtime_lib;
            }
            probe_argv[pk++] = (char *)t->ll_path;
            probe_argv[pk] = NULL;

            rp = run_cmd(probe_argv, cfg->timeout_sec, NULL);
            if (rp.rc != 0 || !parse_probe_timing(rp.stderr_text, &tp_full)) {
                skipped = 1;
                free_cmd_result(&rp);
                break;
            }
            free_cmd_result(&rp);

            l_parse = tp_full.parse_ms;
            l_compile = tp_full.compile_ms;
            l_lookup = tp_full.lookup_ms;

            {
                char *llvm_argv[20];
                int ek = 0;
                llvm_argv[ek++] = (char *)cfg->lli_phases;
                llvm_argv[ek++] = "--json";
                llvm_argv[ek++] = "--no-exec";
                llvm_argv[ek++] = "--func";
                llvm_argv[ek++] = "main";
                llvm_argv[ek++] = "--sig";
                llvm_argv[ek++] = "i32_argc_argv";
                if (cfg->runtime_lib && cfg->runtime_lib[0]) {
                    llvm_argv[ek++] = "--load-lib";
                    llvm_argv[ek++] = (char *)cfg->runtime_lib;
                }
                llvm_argv[ek++] = (char *)t->ll_path;
                llvm_argv[ek] = NULL;
                ri = run_cmd(llvm_argv, cfg->timeout_sec, NULL);
            }

            if (ri.rc != 0 ||
                !json_get_number(ri.stdout_text, "\"parse_ms\"", &e_parse) ||
                !json_get_number(ri.stdout_text, "\"add_module_ms\"", &e_add) ||
                !json_get_number(ri.stdout_text, "\"lookup_ms\"", &e_lookup)) {
                skipped = 1;
                free_cmd_result(&ri);
                break;
            }
            if (!json_get_number(ri.stdout_text, "\"parse_input_ms\"", &e_parse_input))
                e_parse_input = e_parse;
            if (!json_get_number(ri.stdout_text, "\"compile_materialized_ms\"", &e_compile_mat))
                e_compile_mat = e_add + e_lookup;

            free_cmd_result(&ri);

            liric_parse[ok_n] = l_parse;
            liric_compile[ok_n] = l_compile;
            liric_lookup[ok_n] = l_lookup;

            llvm_parse[ok_n] = e_parse;
            llvm_parse_input[ok_n] = e_parse_input;
            llvm_add_module[ok_n] = e_add;
            llvm_lookup[ok_n] = e_lookup;
            llvm_compile_mat[ok_n] = e_compile_mat;

            ok_n++;
        }

        if (!skipped && ok_n > 0) {
            row_t r;
            memset(&r, 0, sizeof(r));
            r.name = xstrdup(t->name);

            r.liric_parse_ms = median(liric_parse, ok_n);
            r.liric_compile_ms = median(liric_compile, ok_n);
            r.liric_lookup_ms = median(liric_lookup, ok_n);
            r.liric_compile_materialized_ms = r.liric_compile_ms + r.liric_lookup_ms;
            r.liric_total_materialized_ms = r.liric_parse_ms + r.liric_compile_materialized_ms;

            r.llvm_parse_ms = median(llvm_parse, ok_n);
            r.llvm_parse_input_ms = median(llvm_parse_input, ok_n);
            r.llvm_add_module_ms = median(llvm_add_module, ok_n);
            r.llvm_lookup_ms = median(llvm_lookup, ok_n);
            r.llvm_compile_materialized_ms = median(llvm_compile_mat, ok_n);
            r.llvm_total_materialized_ms = r.llvm_parse_ms + r.llvm_compile_materialized_ms;

            r.compile_materialized_speedup =
                r.liric_compile_materialized_ms > 0.0
                    ? (r.llvm_compile_materialized_ms / r.liric_compile_materialized_ms)
                    : 0.0;
            r.total_materialized_speedup =
                r.liric_total_materialized_ms > 0.0
                    ? (r.llvm_total_materialized_ms / r.liric_total_materialized_ms)
                    : 0.0;

            fprintf(jf,
                    "{\"name\":\"%s\","
                    "\"liric_parse_median_ms\":%.6f,\"liric_compile_median_ms\":%.6f,"
                    "\"liric_lookup_median_ms\":%.6f,"
                    "\"liric_compile_materialized_median_ms\":%.6f,"
                    "\"liric_total_materialized_median_ms\":%.6f,"
                    "\"llvm_parse_median_ms\":%.6f,"
                    "\"llvm_parse_input_median_ms\":%.6f,"
                    "\"llvm_add_module_median_ms\":%.6f,"
                    "\"llvm_lookup_median_ms\":%.6f,"
                    "\"llvm_compile_materialized_median_ms\":%.6f,"
                    "\"llvm_total_materialized_median_ms\":%.6f,"
                    "\"compile_materialized_speedup\":%.6f,"
                    "\"total_materialized_speedup\":%.6f}\n",
                    t->name,
                    r.liric_parse_ms, r.liric_compile_ms, r.liric_lookup_ms,
                    r.liric_compile_materialized_ms, r.liric_total_materialized_ms,
                    r.llvm_parse_ms, r.llvm_parse_input_ms,
                    r.llvm_add_module_ms, r.llvm_lookup_ms,
                    r.llvm_compile_materialized_ms, r.llvm_total_materialized_ms,
                    r.compile_materialized_speedup, r.total_materialized_speedup);

            rowlist_push(&rows, r);
        } else {
            printf("  [%zu/%zu] %s: skipped\n", i + 1, tests->n, t->name);
        }

        free(liric_parse);
        free(liric_compile);
        free(liric_lookup);
        free(llvm_parse);
        free(llvm_parse_input);
        free(llvm_add_module);
        free(llvm_lookup);
        free(llvm_compile_mat);
    }

    fclose(jf);

    summarize_rows(&rows, out_summary);
    out_summary->attempted = tests->n;

    printf("Corpus compare complete: %zu/%zu\n",
           out_summary->completed,
           out_summary->attempted);

    rowlist_free(&rows);
    return 0;
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    testlist_t tests;
    summary_t summary;

    char *jsonl_path;
    char *summary_path;
    FILE *sf;
    const char *status;

    memset(&summary, 0, sizeof(summary));

    if (mkdir_p(cfg.bench_dir) != 0)
        die("failed to create bench dir", cfg.bench_dir);

    testlist_init(&tests);
    load_corpus_tests(&cfg, &tests);

    if (tests.n == 0) {
        char *empty_summary = path_join2(cfg.bench_dir, "bench_corpus_compare_summary.json");
        FILE *ef = fopen(empty_summary, "w");
        if (ef) {
            fprintf(ef,
                    "{"
                    "\"status\":\"EMPTY DATASET\","
                    "\"dataset_name\":\"corpus_100\","
                    "\"liric_policy\":\"%s\","
                    "\"expected_tests\":100,"
                    "\"attempted_tests\":0"
                    "}\n",
                    cfg.policy);
            fclose(ef);
        }
        fprintf(stderr, "EMPTY DATASET: no corpus tests available\n");
        fprintf(stderr, "  corpus: %s\n", cfg.corpus_tsv);
        fprintf(stderr, "  cache-dir: %s\n", cfg.cache_dir);
        fprintf(stderr, "  bootstrap cache:\n");
        fprintf(stderr, "    ./tools/lfortran_mass/nightly_mass.sh --output-root /tmp/liric_lfortran_mass\n");
        fprintf(stderr, "  override cache location with: --cache-dir PATH\n");
        printf("Status: EMPTY DATASET\n");
        free(empty_summary);
        testlist_free(&tests);
        return cfg.allow_empty ? 0 : 1;
    }

    if (!cfg.runtime_lib || !cfg.runtime_lib[0])
        die("runtime library not found", "(null)");

    jsonl_path = path_join2(cfg.bench_dir, "bench_corpus_compare.jsonl");
    summary_path = path_join2(cfg.bench_dir, "bench_corpus_compare_summary.json");

    run_suite(&cfg, &tests, jsonl_path, &summary);

    if (summary.completed == 0 || summary.attempted == 0)
        status = "EMPTY DATASET";
    else if (summary.completed < summary.attempted)
        status = "PARTIAL";
    else
        status = "OK";

    sf = fopen(summary_path, "w");
    if (!sf) die("failed to open summary", summary_path);

    fprintf(sf,
            "{"
            "\"status\":\"%s\","
            "\"dataset_name\":\"corpus_100\","
            "\"liric_policy\":\"%s\","
            "\"expected_tests\":100,"
            "\"attempted_tests\":%zu,"
            "\"completed_tests\":%zu,"
            "\"liric_parse_median_ms\":%.6f,"
            "\"liric_compile_median_ms\":%.6f,"
            "\"liric_lookup_median_ms\":%.6f,"
            "\"liric_compile_materialized_median_ms\":%.6f,"
            "\"liric_total_materialized_median_ms\":%.6f,"
            "\"llvm_parse_median_ms\":%.6f,"
            "\"llvm_parse_input_median_ms\":%.6f,"
            "\"llvm_add_module_median_ms\":%.6f,"
            "\"llvm_lookup_median_ms\":%.6f,"
            "\"llvm_compile_materialized_median_ms\":%.6f,"
            "\"llvm_total_materialized_median_ms\":%.6f,"
            "\"compile_materialized_speedup_median\":%.6f,"
            "\"total_materialized_speedup_median\":%.6f,"
            "\"compile_materialized_speedup_aggregate\":%.6f,"
            "\"total_materialized_speedup_aggregate\":%.6f,"
            "\"liric_total_materialized_aggregate_ms\":%.6f,"
            "\"llvm_total_materialized_aggregate_ms\":%.6f"
            "}\n",
            status,
            cfg.policy,
            summary.attempted,
            summary.completed,
            summary.liric_parse_median_ms,
            summary.liric_compile_median_ms,
            summary.liric_lookup_median_ms,
            summary.liric_compile_materialized_median_ms,
            summary.liric_total_materialized_median_ms,
            summary.llvm_parse_median_ms,
            summary.llvm_parse_input_median_ms,
            summary.llvm_add_module_median_ms,
            summary.llvm_lookup_median_ms,
            summary.llvm_compile_materialized_median_ms,
            summary.llvm_total_materialized_median_ms,
            summary.compile_materialized_speedup_median,
            summary.total_materialized_speedup_median,
            summary.compile_materialized_speedup_aggregate,
            summary.total_materialized_speedup_aggregate,
            summary.liric_total_materialized_aggregate_ms,
            summary.llvm_total_materialized_aggregate_ms);

    fclose(sf);

    printf("Summary: %s\n", summary_path);
    printf("  completed: %zu/%zu\n", summary.completed, summary.attempted);

    free(jsonl_path);
    free(summary_path);
    testlist_free(&tests);

    if (strcmp(status, "EMPTY DATASET") == 0)
        return cfg.allow_empty ? 0 : 1;
    return 0;
}
