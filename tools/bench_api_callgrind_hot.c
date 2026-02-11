// bench_api_callgrind_hot: summarize API phase ownership timings and
// callgrind hotspots into one JSON artifact.

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct {
    double *v;
    size_t n;
    size_t cap;
} dvec_t;

typedef struct {
    char *symbol;
    unsigned long long ir;
} hot_item_t;

typedef struct {
    hot_item_t *items;
    size_t n;
    size_t cap;
} hot_vec_t;

enum {
    SIDE_LIRIC = 0,
    SIDE_LLVM = 1,
    SIDE_COUNT = 2
};

enum {
    GROUP_BEFORE = 0,   // file->ASR->passes->mod
    GROUP_CODEGEN = 1,  // LLVM IR creation + LLVM opt
    GROUP_BACKEND = 2,  // LLVM->JIT + JIT run
    GROUP_COUNT = 3
};

static const char *k_side_name[SIDE_COUNT] = {"liric", "llvm"};
static const char *k_group_name[GROUP_COUNT] = {
    "lfortran_before_asr_to_mod",
    "lfortran_codegen_llvm_ir_plus_opt",
    "backend_tunable_jit_plus_run"
};

typedef struct {
    const char *bench_jsonl;
    const char *liric_dir;
    const char *llvm_dir;
    const char *out_json;
    int top_n;
} cfg_t;

static void die(const char *msg, const char *arg) {
    if (arg) fprintf(stderr, "%s: %s\n", msg, arg);
    else fprintf(stderr, "%s\n", msg);
    exit(1);
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

static int file_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void dvec_push(dvec_t *v, double x) {
    if (v->n == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        double *nv = (double *)realloc(v->v, ncap * sizeof(double));
        if (!nv) die("out of memory", NULL);
        v->v = nv;
        v->cap = ncap;
    }
    v->v[v->n++] = x;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static double median_of(dvec_t *v) {
    size_t m;
    if (!v || v->n == 0) return 0.0;
    qsort(v->v, v->n, sizeof(double), cmp_double);
    m = v->n / 2;
    if ((v->n % 2) == 1) return v->v[m];
    return 0.5 * (v->v[m - 1] + v->v[m]);
}

static double avg_of(const dvec_t *v) {
    size_t i;
    double s = 0.0;
    if (!v || v->n == 0) return 0.0;
    for (i = 0; i < v->n; i++) s += v->v[i];
    return s / (double)v->n;
}

static void hot_vec_add(hot_vec_t *v, const char *symbol, unsigned long long ir) {
    size_t i;
    for (i = 0; i < v->n; i++) {
        if (strcmp(v->items[i].symbol, symbol) == 0) {
            v->items[i].ir += ir;
            return;
        }
    }
    if (v->n == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 128;
        hot_item_t *nv = (hot_item_t *)realloc(v->items, ncap * sizeof(hot_item_t));
        if (!nv) die("out of memory", NULL);
        v->items = nv;
        v->cap = ncap;
    }
    v->items[v->n].symbol = xstrdup(symbol);
    v->items[v->n].ir = ir;
    v->n++;
}

static int cmp_hot_desc(const void *a, const void *b) {
    const hot_item_t *x = (const hot_item_t *)a;
    const hot_item_t *y = (const hot_item_t *)b;
    if (x->ir < y->ir) return 1;
    if (x->ir > y->ir) return -1;
    return strcmp(x->symbol, y->symbol);
}

static int has_ci(const char *s, const char *needle) {
    size_t i, j, nl;
    if (!s || !needle) return 0;
    nl = strlen(needle);
    if (nl == 0) return 1;
    for (i = 0; s[i]; i++) {
        for (j = 0; j < nl; j++) {
            unsigned char a = (unsigned char)s[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (!a) break;
            if ((unsigned char)tolower(a) != (unsigned char)tolower(b)) break;
        }
        if (j == nl) return 1;
        if (!s[i + j]) return 0;
    }
    return 0;
}

static int classify_group(const char *symbol, int side) {
    if (!symbol) return -1;

    if (has_ci(symbol, "LCompilers::") ||
        has_ci(symbol, "yyparse") ||
        has_ci(symbol, "yyuserAction") ||
        has_ci(symbol, "expr_type0(") ||
        has_ci(symbol, "visit_expr_t<") ||
        has_ci(symbol, "visit_ttype_t<") ||
        has_ci(symbol, "prescan") ||
        has_ci(symbol, "ASR::")) {
        return GROUP_BEFORE;
    }

    if (side == SIDE_LIRIC) {
        if (has_ci(symbol, "lr_") ||
            has_ci(symbol, "/liric/src/target_") ||
            has_ci(symbol, "/liric/src/jit.c") ||
            has_ci(symbol, "/liric/src/objfile")) {
            return GROUP_BACKEND;
        }
    } else {
        if (has_ci(symbol, "llvm::orc") ||
            has_ci(symbol, "ExecutionEngine") ||
            has_ci(symbol, "LLJIT") ||
            has_ci(symbol, "MCJIT") ||
            has_ci(symbol, "LLVM -> JIT")) {
            return GROUP_BACKEND;
        }
    }

    if (has_ci(symbol, "llvm::")) return GROUP_CODEGEN;
    return -1;
}

static int parse_json_number_field(const char *line, const char *key, double *out) {
    char pat[128];
    char *endp = NULL;
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    *out = strtod(p, &endp);
    return endp && endp > p;
}

static void parse_bench_jsonl(const char *path,
                              dvec_t *liric_before, dvec_t *llvm_before,
                              dvec_t *liric_codegen, dvec_t *llvm_codegen,
                              dvec_t *liric_backend, dvec_t *llvm_backend,
                              size_t *ok_rows) {
    FILE *f = fopen(path, "r");
    char line[16384];
    if (!f) die("failed to open bench_api jsonl", path);
    *ok_rows = 0;
    while (fgets(line, sizeof(line), f)) {
        double x1, x2, x3, x4, x5, x6;
        if (!strstr(line, "\"status\":\"ok\"")) continue;
        if (!parse_json_number_field(line, "liric_before_asr_to_mod_median_ms", &x1)) continue;
        if (!parse_json_number_field(line, "llvm_before_asr_to_mod_median_ms", &x2)) continue;
        if (!parse_json_number_field(line, "liric_codegen_median_ms", &x3)) continue;
        if (!parse_json_number_field(line, "llvm_codegen_median_ms", &x4)) continue;
        if (!parse_json_number_field(line, "liric_backend_median_ms", &x5)) continue;
        if (!parse_json_number_field(line, "llvm_backend_median_ms", &x6)) continue;
        dvec_push(liric_before, x1);
        dvec_push(llvm_before, x2);
        dvec_push(liric_codegen, x3);
        dvec_push(llvm_codegen, x4);
        dvec_push(liric_backend, x5);
        dvec_push(llvm_backend, x6);
        (*ok_rows)++;
    }
    fclose(f);
}

static unsigned long long parse_ir_with_commas(const char *s, const char **after) {
    unsigned long long v = 0;
    const char *p = s;
    int saw_digit = 0;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            saw_digit = 1;
            v = v * 10ULL + (unsigned long long)(*p - '0');
        } else if (*p == ',') {
            // skip
        } else {
            break;
        }
        p++;
    }
    if (!saw_digit) return 0;
    if (after) *after = p;
    return v;
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static void simplify_symbol(char *sym) {
    size_t n;
    char *bin;
    char *last_slash;
    char *last_colon;
    if (!sym || !sym[0]) return;
    if (strncmp(sym, "???:", 4) == 0)
        memmove(sym, sym + 4, strlen(sym + 4) + 1);
    bin = strstr(sym, " [/");
    if (bin) *bin = '\0';
    last_slash = strrchr(sym, '/');
    last_colon = strrchr(sym, ':');
    if (last_slash && last_colon && last_colon > last_slash)
        memmove(sym, last_colon + 1, strlen(last_colon + 1) + 1);
    n = strlen(sym);
    if (n > 180) {
        sym[177] = '.';
        sym[178] = '.';
        sym[179] = '.';
        sym[180] = '\0';
    }
}

static void collect_hot_from_trace(const char *trace_path,
                                   int side,
                                   hot_vec_t hot[SIDE_COUNT][GROUP_COUNT]) {
    char cmd[PATH_MAX + 128];
    FILE *p;
    char line[16384];
    int in_table = 0;
    int table_rows = 0;

    snprintf(cmd, sizeof(cmd), "callgrind_annotate --auto=no --threshold=100 '%s'", trace_path);
    p = popen(cmd, "r");
    if (!p) return;

    while (fgets(line, sizeof(line), p)) {
        char *sym;
        const char *after_ir;
        const char *paren;
        unsigned long long ir;
        int group;

        if (!in_table) {
            if (strstr(line, "file:function")) in_table = 1;
            continue;
        }
        if (table_rows > 500) break;
        if (line[0] == '\n' || line[0] == '\r') continue;
        if (!isdigit((unsigned char)line[0]) && line[0] != ' ') continue;

        while (*line == ' ' || *line == '\t') memmove(line, line + 1, strlen(line));
        ir = parse_ir_with_commas(line, &after_ir);
        if (ir == 0) continue;
        paren = strstr(after_ir, ")  ");
        if (!paren) continue;
        sym = xstrdup(paren + 3);
        rstrip(sym);
        simplify_symbol(sym);
        if (!sym[0]) {
            free(sym);
            continue;
        }
        group = classify_group(sym, side);
        if (group >= 0 && group < GROUP_COUNT) {
            hot_vec_add(&hot[side][group], sym, ir);
        }
        free(sym);
        table_rows++;
    }
    pclose(p);
}

static void collect_dir_hotspots(const char *dir_path,
                                 int side,
                                 hot_vec_t hot[SIDE_COUNT][GROUP_COUNT],
                                 size_t *trace_count) {
    DIR *d = opendir(dir_path);
    struct dirent *ent;
    char path[PATH_MAX];
    *trace_count = 0;
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 4) continue;
        if (strcmp(ent->d_name + n - 3, ".cg") != 0) continue;
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name) <= 0) continue;
        collect_hot_from_trace(path, side, hot);
        (*trace_count)++;
    }
    closedir(d);
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    cfg.bench_jsonl = "/tmp/liric_bench/bench_api.jsonl";
    cfg.liric_dir = "/tmp/liric_bench_callgrind/callgrind/liric";
    cfg.llvm_dir = "/tmp/liric_bench_callgrind/callgrind/llvm";
    cfg.out_json = "/tmp/liric_bench_callgrind/bench_api_callgrind_phase_hot.json";
    cfg.top_n = 8;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench-jsonl") == 0 && i + 1 < argc) {
            cfg.bench_jsonl = argv[++i];
        } else if (strcmp(argv[i], "--liric-dir") == 0 && i + 1 < argc) {
            cfg.liric_dir = argv[++i];
        } else if (strcmp(argv[i], "--llvm-dir") == 0 && i + 1 < argc) {
            cfg.llvm_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            cfg.out_json = argv[++i];
        } else if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            cfg.top_n = atoi(argv[++i]);
            if (cfg.top_n <= 0) cfg.top_n = 8;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: bench_api_callgrind_hot [options]\n");
            printf("  --bench-jsonl PATH   bench_api jsonl with phase split fields\n");
            printf("  --liric-dir PATH     callgrind trace dir for liric-side runs\n");
            printf("  --llvm-dir PATH      callgrind trace dir for llvm-side runs\n");
            printf("  --out PATH           output json\n");
            printf("  --top N              top symbols per group/side (default: 8)\n");
            exit(0);
        } else {
            die("unknown argument", argv[i]);
        }
    }
    if (!file_exists(cfg.bench_jsonl)) die("bench jsonl not found", cfg.bench_jsonl);
    if (!is_dir(cfg.liric_dir)) die("liric callgrind dir not found", cfg.liric_dir);
    if (!is_dir(cfg.llvm_dir)) die("llvm callgrind dir not found", cfg.llvm_dir);
    return cfg;
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    dvec_t liric_before = {0}, llvm_before = {0};
    dvec_t liric_codegen = {0}, llvm_codegen = {0};
    dvec_t liric_backend = {0}, llvm_backend = {0};
    hot_vec_t hot[SIDE_COUNT][GROUP_COUNT] = {{{0}}};
    size_t ok_rows = 0;
    size_t trace_counts[SIDE_COUNT] = {0, 0};
    size_t s, g;
    FILE *out;

    parse_bench_jsonl(cfg.bench_jsonl,
                      &liric_before, &llvm_before,
                      &liric_codegen, &llvm_codegen,
                      &liric_backend, &llvm_backend,
                      &ok_rows);
    collect_dir_hotspots(cfg.liric_dir, SIDE_LIRIC, hot, &trace_counts[SIDE_LIRIC]);
    collect_dir_hotspots(cfg.llvm_dir, SIDE_LLVM, hot, &trace_counts[SIDE_LLVM]);

    out = fopen(cfg.out_json, "w");
    if (!out) die("failed to open output", cfg.out_json);

    fprintf(out, "{\n");
    fprintf(out, "  \"ok_rows\": %zu,\n", ok_rows);
    fprintf(out, "  \"trace_counts\": {\"liric\": %zu, \"llvm\": %zu},\n",
            trace_counts[SIDE_LIRIC], trace_counts[SIDE_LLVM]);
    fprintf(out, "  \"timings_ms\": {\n");
    fprintf(out, "    \"lfortran_before_asr_to_mod\": {\n");
    fprintf(out, "      \"liric_median\": %.6f,\n", median_of(&liric_before));
    fprintf(out, "      \"llvm_median\": %.6f,\n", median_of(&llvm_before));
    fprintf(out, "      \"liric_avg\": %.6f,\n", avg_of(&liric_before));
    fprintf(out, "      \"llvm_avg\": %.6f\n", avg_of(&llvm_before));
    fprintf(out, "    },\n");
    fprintf(out, "    \"lfortran_codegen_llvm_ir_plus_opt\": {\n");
    fprintf(out, "      \"liric_median\": %.6f,\n", median_of(&liric_codegen));
    fprintf(out, "      \"llvm_median\": %.6f,\n", median_of(&llvm_codegen));
    fprintf(out, "      \"liric_avg\": %.6f,\n", avg_of(&liric_codegen));
    fprintf(out, "      \"llvm_avg\": %.6f\n", avg_of(&llvm_codegen));
    fprintf(out, "    },\n");
    fprintf(out, "    \"backend_tunable_jit_plus_run\": {\n");
    fprintf(out, "      \"liric_median\": %.6f,\n", median_of(&liric_backend));
    fprintf(out, "      \"llvm_median\": %.6f,\n", median_of(&llvm_backend));
    fprintf(out, "      \"liric_avg\": %.6f,\n", avg_of(&liric_backend));
    fprintf(out, "      \"llvm_avg\": %.6f\n", avg_of(&llvm_backend));
    fprintf(out, "    }\n");
    fprintf(out, "  },\n");
    fprintf(out, "  \"hot_paths\": {\n");
    for (g = 0; g < GROUP_COUNT; g++) {
        fprintf(out, "    \"%s\": {\n", k_group_name[g]);
        for (s = 0; s < SIDE_COUNT; s++) {
            size_t i, lim;
            qsort(hot[s][g].items, hot[s][g].n, sizeof(hot_item_t), cmp_hot_desc);
            lim = hot[s][g].n < (size_t)cfg.top_n ? hot[s][g].n : (size_t)cfg.top_n;
            fprintf(out, "      \"%s\": [", k_side_name[s]);
            for (i = 0; i < lim; i++) {
                fprintf(out, "{\"symbol\":\"");
                {
                    const char *p = hot[s][g].items[i].symbol;
                    while (*p) {
                        if (*p == '\\' || *p == '"') fputc('\\', out);
                        fputc(*p, out);
                        p++;
                    }
                }
                fprintf(out, "\",\"ir\":%llu}%s",
                        hot[s][g].items[i].ir,
                        (i + 1 == lim) ? "" : ",");
            }
            fprintf(out, "]%s\n", (s + 1 == SIDE_COUNT) ? "" : ",");
        }
        fprintf(out, "    }%s\n", (g + 1 == GROUP_COUNT) ? "" : ",");
    }
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
    fclose(out);

    printf("Wrote %s\n", cfg.out_json);
    return 0;
}
