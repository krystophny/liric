// Compatibility check: compare liric JIT and lli output against lfortran native output.
// C replacement for tools/bench_compat_check.py.

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#include "bench_common.h"

typedef struct {
    char **items;
    size_t n;
    size_t cap;
} strlist_t;

typedef bench_cmd_result_t cmd_result_t;

typedef struct {
    char *name;
    char *source;
    strlist_t labels;
    strlist_t extrafiles;
    strlist_t extra_args;
    char *options_joined;
    int expected_fail;
    int llvm;
} test_entry_t;

typedef struct {
    test_entry_t *items;
    size_t n;
    size_t cap;
} testlist_t;

typedef struct {
    const char *lfortran;
    const char *probe_runner;
    const char *runtime_lib;
    const char *lli;
    const char *cmake;
    const char *bench_dir;
    int timeout_sec;
    int limit;
    int freeze_api_n;
} cfg_t;

typedef struct {
    char *name;
    char *options;
    char *source;
} name_opt_t;

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

static int is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void ensure_dir(const char *path) {
    if (is_dir(path)) return;
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        die("failed to create dir %s: %s", path, strerror(errno));
    }
}

static char *xstrdup(const char *s) {
    char *p;
    p = bench_xstrdup(s);
    if (!p && s) die("out of memory");
    return p;
}

static char *to_abs_path(const char *path) {
    char *out;
    out = bench_to_abs_path(path);
    if (!out && path) die("failed to resolve absolute path %s", path);
    return out;
}

static void strlist_init(strlist_t *l) {
    l->items = NULL;
    l->n = 0;
    l->cap = 0;
}

static void strlist_push(strlist_t *l, const char *s) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 8;
        char **nitems = (char **)realloc(l->items, ncap * sizeof(char *));
        if (!nitems) die("out of memory");
        l->items = nitems;
        l->cap = ncap;
    }
    l->items[l->n++] = xstrdup(s);
}

static int strlist_contains(const strlist_t *l, const char *s) {
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (strcmp(l->items[i], s) == 0) return 1;
    }
    return 0;
}

static void strlist_push_unique(strlist_t *l, const char *s) {
    if (!strlist_contains(l, s)) strlist_push(l, s);
}

static void strlist_free(strlist_t *l) {
    size_t i;
    for (i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static char *path_join2(const char *a, const char *b) {
    char *out = bench_path_join2(a, b);
    if (!out) die("out of memory");
    return out;
}

static char *dirname_dup(const char *path) {
    char *out = bench_dirname_dup(path);
    if (!out) die("out of memory");
    return out;
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
    if (!out) die("out of memory");
    p = out;
    for (i = 0; s && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') { *p++='\\'; *p++='\\'; }
        else if (c == '"') { *p++='\\'; *p++='"'; }
        else if (c == '\n') { *p++='\\'; *p++='n'; }
        else if (c == '\r') { *p++='\\'; *p++='r'; }
        else if (c == '\t') { *p++='\\'; *p++='t'; }
        else if (c < 0x20) {
            sprintf(p, "\\u%04x", c);
            p += 6;
        } else *p++ = (char)c;
    }
    *p = '\0';
    return out;
}

static char *normalize_output(const char *s) {
    size_t n = strlen(s), i = 0;
    char *tmp = (char *)malloc(n + 1);
    char *out = NULL;
    size_t t = 0;
    if (!tmp) die("out of memory");

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

    while (t > 0 && (tmp[t - 1] == '\n')) t--;
    out = (char *)malloc(t + 1);
    if (!out) die("out of memory");
    memcpy(out, tmp, t);
    out[t] = '\0';
    free(tmp);
    return out;
}

static cmd_result_t run_cmd(char *const argv[], int timeout_sec, const char *stdout_path,
                            const char *env_lib_dir, const char *work_dir) {
    bench_run_cmd_opts_t opts;
    cmd_result_t r;
    memset(&opts, 0, sizeof(opts));
    opts.argv = argv;
    opts.timeout_ms = timeout_sec > 0 ? timeout_sec * 1000 : 0;
    opts.stdout_path = stdout_path;
    opts.env_lib_dir = env_lib_dir;
    opts.work_dir = work_dir;
    if (bench_run_cmd(&opts, &r) != 0) die("failed to run command: %s", argv[0]);
    return r;
}

static void free_cmd_result(cmd_result_t *r) {
    bench_free_cmd_result(r);
}

static char *strip_comments_keep_quotes(const char *text) {
    size_t n = strlen(text), i = 0, o = 0;
    int in_quote = 0;
    char *out = (char *)malloc(n + 1);
    if (!out) die("out of memory");

    while (i < n) {
        char c = text[i];
        if (c == '"') {
            in_quote = !in_quote;
            out[o++] = c;
            i++;
            continue;
        }
        if (c == '#' && !in_quote) {
            while (i < n && text[i] != '\n') i++;
            continue;
        }
        out[o++] = c;
        i++;
    }
    out[o] = '\0';
    return out;
}

static strlist_t tokenize_shell_like(const char *s) {
    strlist_t toks;
    size_t i = 0, n = strlen(s);
    strlist_init(&toks);

    while (i < n) {
        char *tok;
        size_t t = 0;
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i >= n) break;

        tok = (char *)malloc(n - i + 1);
        if (!tok) die("out of memory");

        if (s[i] == '"') {
            i++;
            while (i < n) {
                if (s[i] == '"') {
                    i++;
                    break;
                }
                if (s[i] == '\\' && i + 1 < n) {
                    i++;
                    tok[t++] = s[i++];
                    continue;
                }
                tok[t++] = s[i++];
            }
        } else {
            while (i < n && !isspace((unsigned char)s[i])) {
                tok[t++] = s[i++];
            }
        }
        tok[t] = '\0';
        strlist_push(&toks, tok);
        free(tok);
    }

    return toks;
}

static char *join_tokens_shell_escaped(const strlist_t *toks) {
    size_t i;
    size_t n = 0;
    char *out, *p;
    for (i = 0; i < toks->n; i++) {
        const char *s = toks->items[i];
        int needs_quote = 0;
        size_t j;
        for (j = 0; s[j]; j++) {
            if (isspace((unsigned char)s[j]) || s[j] == '\'' || s[j] == '"') {
                needs_quote = 1;
                break;
            }
        }
        if (i) n++;
        if (!needs_quote) n += strlen(s);
        else {
            n += 2;
            for (j = 0; s[j]; j++) n += (s[j] == '\'' ? 4 : 1);
        }
    }

    out = (char *)malloc(n + 1);
    if (!out) die("out of memory");
    p = out;
    for (i = 0; i < toks->n; i++) {
        const char *s = toks->items[i];
        int needs_quote = 0;
        size_t j;
        if (i) *p++ = ' ';
        for (j = 0; s[j]; j++) {
            if (isspace((unsigned char)s[j]) || s[j] == '\'' || s[j] == '"') {
                needs_quote = 1;
                break;
            }
        }
        if (!needs_quote) {
            size_t m = strlen(s);
            memcpy(p, s, m);
            p += m;
        } else {
            *p++ = '\'';
            for (j = 0; s[j]; j++) {
                if (s[j] == '\'') {
                    memcpy(p, "'\\''", 4);
                    p += 4;
                } else {
                    *p++ = s[j];
                }
            }
            *p++ = '\'';
        }
    }
    *p = '\0';
    return out;
}

static int is_flag_key(const char *tok) {
    static const char *keys[] = {
        "FAIL", "NOFAST_TILL_LLVM16", "NO_FAST", "NO_STD_F23", "OLD_CLASSES", "NO_LLVM_GOC"
    };
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(tok, keys[i]) == 0) return 1;
    }
    return 0;
}

static int is_one_value_key(const char *tok) {
    return strcmp(tok, "NAME") == 0 || strcmp(tok, "FILE") == 0 ||
           strcmp(tok, "INCLUDE_PATH") == 0 || strcmp(tok, "COPY_TO_BIN") == 0;
}

static int multi_key_kind(const char *tok) {
    if (strcmp(tok, "LABELS") == 0) return 1;
    if (strcmp(tok, "EXTRAFILES") == 0) return 2;
    if (strcmp(tok, "EXTRA_ARGS") == 0) return 3;
    if (strcmp(tok, "GFORTRAN_ARGS") == 0) return 4;
    return 0;
}

static int has_suffix(const char *name) {
    const char *slash = strrchr(name, '/');
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    if (slash && dot < slash) return 0;
    return 1;
}

static void append_llvm_label_options(strlist_t *opts, const strlist_t *labels) {
    if (strlist_contains(labels, "llvmImplicit")) {
        strlist_push_unique(opts, "--implicit-typing");
        strlist_push_unique(opts, "--implicit-interface");
    }
    if (strlist_contains(labels, "llvmStackArray")) {
        strlist_push_unique(opts, "--stack-arrays=true");
    }
    if (strlist_contains(labels, "llvm_integer_8")) {
        strlist_push_unique(opts, "-fdefault-integer-8");
    }
    if (strlist_contains(labels, "llvm_nopragma")) {
        strlist_push_unique(opts, "--ignore-pragma");
    }
    if (strlist_contains(labels, "llvm_omp")) {
        strlist_push_unique(opts, "--openmp");
    }
    if (strlist_contains(labels, "llvm2")) {
        strlist_push_unique(opts, "--separate-compilation");
    }
    if (strlist_contains(labels, "llvm_rtlib")) {
        strlist_push_unique(opts, "--separate-compilation");
        strlist_push_unique(opts, "--rtlib");
    }
}

static void testlist_push(testlist_t *l, test_entry_t e) {
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 64;
        test_entry_t *nitems = (test_entry_t *)realloc(l->items, ncap * sizeof(test_entry_t));
        if (!nitems) die("out of memory");
        l->items = nitems;
        l->cap = ncap;
    }
    l->items[l->n++] = e;
}

static void free_test_entry(test_entry_t *e) {
    free(e->name);
    free(e->source);
    strlist_free(&e->labels);
    strlist_free(&e->extrafiles);
    strlist_free(&e->extra_args);
    free(e->options_joined);
}

static void free_testlist(testlist_t *l) {
    size_t i;
    for (i = 0; i < l->n; i++) free_test_entry(&l->items[i]);
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

static testlist_t parse_integration_runs(const char *cmake_path) {
    char *text = bench_read_all_file(cmake_path);
    char *clean = strip_comments_keep_quotes(text);
    char *integration_dir = dirname_dup(cmake_path);
    size_t i = 0, n = strlen(clean);
    testlist_t out;

    out.items = NULL;
    out.n = 0;
    out.cap = 0;

    while (i < n) {
        if (i + 3 <= n && strncmp(clean + i, "RUN", 3) == 0) {
            size_t j = i + 3;
            while (j < n && isspace((unsigned char)clean[j])) j++;
            if (j < n && clean[j] == '(') {
                size_t start = ++j;
                int depth = 1;
                int in_quote = 0;
                while (j < n && depth > 0) {
                    char c = clean[j++];
                    if (c == '"') in_quote = !in_quote;
                    if (!in_quote) {
                        if (c == '(') depth++;
                        else if (c == ')') depth--;
                    }
                }
                if (depth == 0) {
                    size_t end = j - 1;
                    char *block = (char *)malloc(end - start + 1);
                    strlist_t toks;
                    test_entry_t e;
                    size_t k;
                    int current_multi = 0;
                    char *name = NULL;
                    char *file_tok = NULL;

                    if (!block) die("out of memory");
                    memcpy(block, clean + start, end - start);
                    block[end - start] = '\0';

                    toks = tokenize_shell_like(block);

                    memset(&e, 0, sizeof(e));
                    strlist_init(&e.labels);
                    strlist_init(&e.extrafiles);
                    strlist_init(&e.extra_args);
                    e.expected_fail = 0;
                    e.llvm = 0;

                    for (k = 0; k < toks.n; k++) {
                        const char *tok = toks.items[k];
                        int mk;
                        if (is_flag_key(tok)) {
                            if (strcmp(tok, "FAIL") == 0) e.expected_fail = 1;
                            current_multi = 0;
                            continue;
                        }
                        if (is_one_value_key(tok)) {
                            if (k + 1 < toks.n) {
                                const char *val = toks.items[++k];
                                if (strcmp(tok, "NAME") == 0) {
                                    free(name);
                                    name = xstrdup(val);
                                } else if (strcmp(tok, "FILE") == 0) {
                                    free(file_tok);
                                    file_tok = xstrdup(val);
                                }
                            }
                            current_multi = 0;
                            continue;
                        }
                        mk = multi_key_kind(tok);
                        if (mk) {
                            current_multi = mk;
                            continue;
                        }
                        if (current_multi == 1) strlist_push(&e.labels, tok);
                        else if (current_multi == 2) strlist_push(&e.extrafiles, tok);
                        else if (current_multi == 3) strlist_push(&e.extra_args, tok);
                    }

                    if (name) {
                        char *src;
                        if (!file_tok) file_tok = xstrdup(name);
                        if (!has_suffix(file_tok)) {
                            char *tmp = (char *)malloc(strlen(file_tok) + 5);
                            sprintf(tmp, "%s.f90", file_tok);
                            free(file_tok);
                            file_tok = tmp;
                        }
                        src = path_join2(integration_dir, file_tok);
                        append_llvm_label_options(&e.extra_args, &e.labels);

                        e.name = name;
                        e.source = src;
                        e.options_joined = join_tokens_shell_escaped(&e.extra_args);
                        e.llvm = strlist_contains(&e.labels, "llvm");
                        testlist_push(&out, e);
                    } else {
                        free_test_entry(&e);
                        free(name);
                    }

                    free(file_tok);
                    strlist_free(&toks);
                    free(block);
                    i = j;
                    continue;
                }
            }
        }
        i++;
    }

    free(integration_dir);
    free(clean);
    free(text);
    return out;
}

static char **make_argv(size_t n) {
    char **argv = (char **)calloc(n + 1, sizeof(char *));
    if (!argv) die("out of memory");
    return argv;
}

static void write_json_row(FILE *f,
                           const char *name,
                           const char *source,
                           const char *options,
                           int llvm_ok,
                           int liric_ok,
                           int lli_ok,
                           int liric_match,
                           int lli_match,
                           int liric_rc_match,
                           int lli_rc_match,
                           int llvm_rc,
                           int liric_rc,
                           int lli_rc,
                           const char *error) {
    char *e_name = json_escape(name ? name : "");
    char *e_source = json_escape(source ? source : "");
    char *e_options = json_escape(options ? options : "");
    char *e_error = json_escape(error ? error : "");

    fprintf(f,
        "{\"name\":\"%s\",\"source\":\"%s\",\"options\":\"%s\","
        "\"llvm_ok\":%s,\"liric_ok\":%s,\"lli_ok\":%s,"
        "\"liric_match\":%s,\"lli_match\":%s,"
        "\"liric_rc_match\":%s,\"lli_rc_match\":%s,"
        "\"llvm_rc\":%d,\"liric_rc\":%d,\"lli_rc\":%d,\"error\":\"%s\"}\n",
        e_name, e_source, e_options,
        llvm_ok ? "true" : "false",
        liric_ok ? "true" : "false",
        lli_ok ? "true" : "false",
        liric_match ? "true" : "false",
        lli_match ? "true" : "false",
        liric_rc_match ? "true" : "false",
        lli_rc_match ? "true" : "false",
        llvm_rc, liric_rc, lli_rc, e_error);

    free(e_name);
    free(e_source);
    free(e_options);
    free(e_error);
}

static int cmp_strptr(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static int cmp_name_opt(const void *a, const void *b) {
    const name_opt_t *na = (const name_opt_t *)a;
    const name_opt_t *nb = (const name_opt_t *)b;
    return strcmp(na->name, nb->name);
}

static const name_opt_t *find_option_by_name(const name_opt_t *opts, size_t n, const char *name) {
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(opts[mid].name, name);
        if (cmp == 0) return &opts[mid];
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static void usage(void) {
    printf("usage: bench_compat_check [options]\n");
    printf("  --workers N            (ignored, kept for compatibility)\n");
    printf("  --timeout N            command timeout in seconds (default: 15)\n");
    printf("  --limit N              limit number of tests (default: 0 = all)\n");
    printf("  --bench-dir PATH       output directory (default: /tmp/liric_bench)\n");
    printf("  --freeze-api N         frozen compat corpus size (default: 100)\n");
    printf("  --lfortran PATH        path to lfortran binary\n");
    printf("  --probe-runner PATH    path to liric_probe_runner\n");
    printf("  --runtime-lib PATH     path to liblfortran_runtime (used by lli and liric)\n");
    printf("  --lli PATH             path to lli (default: lli)\n");
    printf("  --cmake PATH           path to integration_tests/CMakeLists.txt\n");
}

static cfg_t parse_args(int argc, char **argv) {
    cfg_t cfg;
    int i;
    const char *default_runtime_dylib = "build/deps/lfortran/build-llvm/src/runtime/liblfortran_runtime.dylib";
    const char *default_runtime_so = "build/deps/lfortran/build-llvm/src/runtime/liblfortran_runtime.so";

    cfg.lfortran = "build/deps/lfortran/build-llvm/src/bin/lfortran";
    cfg.probe_runner = "build/liric_probe_runner";
    cfg.runtime_lib = file_exists(default_runtime_dylib) ? default_runtime_dylib : default_runtime_so;
    cfg.lli = "lli";
    cfg.cmake = "build/deps/lfortran/integration_tests/CMakeLists.txt";
    cfg.bench_dir = "/tmp/liric_bench";
    cfg.timeout_sec = 15;
    cfg.limit = 0;
    cfg.freeze_api_n = 100;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            cfg.timeout_sec = atoi(argv[++i]);
            if (cfg.timeout_sec <= 0) cfg.timeout_sec = 15;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            cfg.limit = atoi(argv[++i]);
            if (cfg.limit < 0) cfg.limit = 0;
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            cfg.bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--freeze-api") == 0 && i + 1 < argc) {
            cfg.freeze_api_n = atoi(argv[++i]);
            if (cfg.freeze_api_n <= 0) cfg.freeze_api_n = 100;
        } else if (strcmp(argv[i], "--lfortran") == 0 && i + 1 < argc) {
            cfg.lfortran = argv[++i];
        } else if (strcmp(argv[i], "--probe-runner") == 0 && i + 1 < argc) {
            cfg.probe_runner = argv[++i];
        } else if (strcmp(argv[i], "--runtime-lib") == 0 && i + 1 < argc) {
            cfg.runtime_lib = argv[++i];
        } else if (strcmp(argv[i], "--lli") == 0 && i + 1 < argc) {
            cfg.lli = argv[++i];
        } else if (strcmp(argv[i], "--cmake") == 0 && i + 1 < argc) {
            cfg.cmake = argv[++i];
        } else {
            die("unknown argument: %s", argv[i]);
        }
    }

    if (!file_exists(cfg.lfortran)) die("lfortran not found: %s", cfg.lfortran);
    if (!file_exists(cfg.probe_runner)) die("probe runner not found: %s", cfg.probe_runner);
    if (!file_exists(cfg.runtime_lib)) die("runtime lib not found: %s", cfg.runtime_lib);
    if (!file_exists(cfg.cmake)) die("cmake file not found: %s", cfg.cmake);

    cfg.lfortran = to_abs_path(cfg.lfortran);
    cfg.probe_runner = to_abs_path(cfg.probe_runner);
    cfg.runtime_lib = to_abs_path(cfg.runtime_lib);
    cfg.cmake = to_abs_path(cfg.cmake);
    cfg.bench_dir = to_abs_path(cfg.bench_dir);
    if (strchr(cfg.lli, '/')) cfg.lli = to_abs_path(cfg.lli);

    return cfg;
}

int main(int argc, char **argv) {
    cfg_t cfg = parse_args(argc, argv);
    testlist_t tests = parse_integration_runs(cfg.cmake);
    char *ll_dir = path_join2(cfg.bench_dir, "ll");
    char *bin_dir = path_join2(cfg.bench_dir, "bin");
    char *jsonl_path = path_join2(cfg.bench_dir, "compat_check.jsonl");
    char *compat_api_path = path_join2(cfg.bench_dir, "compat_api.txt");
    char *compat_ll_path = path_join2(cfg.bench_dir, "compat_ll.txt");
    char *opts_api_path = path_join2(cfg.bench_dir, "compat_api_options.jsonl");
    char *opts_ll_path = path_join2(cfg.bench_dir, "compat_ll_options.jsonl");
    char frozen_list_name[64];
    char frozen_opts_name[64];
    char *frozen_api_path;
    char *frozen_opts_path;
    char *runtime_dir = dirname_dup(cfg.runtime_lib);
    FILE *jsonl = NULL;
    char **compat_api = NULL;
    char **compat_ll = NULL;
    name_opt_t *opts_api = NULL;
    name_opt_t *opts_ll = NULL;
    size_t compat_api_n = 0, compat_ll_n = 0, opts_api_n = 0, opts_ll_n = 0;
    size_t compat_api_cap = 0, compat_ll_cap = 0, opts_api_cap = 0, opts_ll_cap = 0;
    size_t i;
    size_t eligible = 0;
    size_t processed = 0;
    size_t llvm_ok_count = 0;
    size_t liric_match_count = 0;
    size_t lli_match_count = 0;
    size_t both_match_count = 0;

    snprintf(frozen_list_name, sizeof(frozen_list_name), "compat_api_%d.txt", cfg.freeze_api_n);
    snprintf(frozen_opts_name, sizeof(frozen_opts_name), "compat_api_%d_options.jsonl", cfg.freeze_api_n);
    frozen_api_path = path_join2(cfg.bench_dir, frozen_list_name);
    frozen_opts_path = path_join2(cfg.bench_dir, frozen_opts_name);

    ensure_dir(cfg.bench_dir);
    ensure_dir(ll_dir);
    ensure_dir(bin_dir);

    for (i = 0; i < tests.n; i++) {
        test_entry_t *t = &tests.items[i];
        if (!t->llvm) continue;
        if (t->expected_fail) continue;
        if (t->extrafiles.n > 0) continue;
        if (strlist_contains(&t->labels, "llvm_omp") ||
            strlist_contains(&t->labels, "llvm2") ||
            strlist_contains(&t->labels, "llvm_rtlib")) continue;
        if (!file_exists(t->source)) continue;
        eligible++;
    }

    printf("Found %zu eligible integration tests\n", eligible);
    printf("timeout: %ds\n", cfg.timeout_sec);

#define UPDATE_STATS_AND_PROGRESS() \
    do { \
        processed++; \
        if (llvm_ok) llvm_ok_count++; \
        if (liric_match) liric_match_count++; \
        if (lli_match) lli_match_count++; \
        if (liric_match && lli_match) both_match_count++; \
        if (processed % 50 == 0) { \
            printf("  progress %zu/%zu: llvm_ok=%zu liric_match=%zu (%.1f%%) lli_match=%zu both=%zu\n", \
                   processed, eligible, \
                   llvm_ok_count, \
                   liric_match_count, processed ? (100.0 * (double)liric_match_count / (double)processed) : 0.0, \
                   lli_match_count, \
                   both_match_count); \
        } \
    } while (0)

    jsonl = fopen(jsonl_path, "w");
    if (!jsonl) die("failed to open %s", jsonl_path);

    for (i = 0; i < tests.n; i++) {
        test_entry_t *t = &tests.items[i];
        char *ll_path;
        char *bin_path;
        char **emit_argv;
        char **compile_argv;
        char *const *run_argv;
        char *const *jit_argv;
        char *const *lli_argv;
        cmd_result_t emit_r, compile_r, run_r, jit_r, lli_r;
        int llvm_ok = 0, liric_ok = 0, lli_ok = 0;
        int liric_match = 0, lli_match = 0;
        int liric_rc_match = 0, lli_rc_match = 0;
        int llvm_rc = -1, liric_rc = -1, lli_rc = -1;
        char *error = xstrdup("");
        char *n_llvm_out = NULL, *n_liric_out = NULL, *n_lli_out = NULL;
        char work_tpl[PATH_MAX];
        const char *work_dir = NULL;
        size_t j;

        if (!t->llvm || t->expected_fail || t->extrafiles.n > 0 ||
            strlist_contains(&t->labels, "llvm_omp") ||
            strlist_contains(&t->labels, "llvm2") ||
            strlist_contains(&t->labels, "llvm_rtlib") ||
            !file_exists(t->source)) {
            continue;
        }

        if (cfg.limit > 0 && (int)processed >= cfg.limit) break;

        {
            int n = snprintf(work_tpl, sizeof(work_tpl), "%s/%s", cfg.bench_dir, "work_compat_XXXXXX");
            if (n < 0 || (size_t)n >= sizeof(work_tpl)) {
                free(error);
                error = xstrdup("work dir template too long");
                write_json_row(jsonl, t->name, t->source, t->options_joined,
                               llvm_ok, liric_ok, lli_ok, liric_match, lli_match,
                               liric_rc_match, lli_rc_match,
                               llvm_rc, liric_rc, lli_rc, error);
                UPDATE_STATS_AND_PROGRESS();
                free(error);
                continue;
            }
        }
        if (!mkdtemp(work_tpl)) {
            free(error);
            error = xstrdup("failed to create temp work dir");
            write_json_row(jsonl, t->name, t->source, t->options_joined,
                           llvm_ok, liric_ok, lli_ok, liric_match, lli_match,
                           liric_rc_match, lli_rc_match,
                           llvm_rc, liric_rc, lli_rc, error);
            UPDATE_STATS_AND_PROGRESS();
            free(error);
            continue;
        }
        work_dir = work_tpl;

        ll_path = path_join2(ll_dir, t->name);
        {
            char *tmp = ll_path;
            ll_path = (char *)malloc(strlen(tmp) + 4);
            sprintf(ll_path, "%s.ll", tmp);
            free(tmp);
        }
        bin_path = path_join2(bin_dir, t->name);

        emit_argv = make_argv(4 + t->extra_args.n + 2);
        emit_argv[0] = (char *)cfg.lfortran;
        emit_argv[1] = "--no-color";
        emit_argv[2] = "--show-llvm";
        for (j = 0; j < t->extra_args.n; j++) emit_argv[3 + j] = t->extra_args.items[j];
        emit_argv[3 + t->extra_args.n] = t->source;
        emit_argv[4 + t->extra_args.n] = NULL;

        emit_r = run_cmd(emit_argv, cfg.timeout_sec, ll_path, NULL, work_dir);
        if (emit_r.rc != 0 || !file_exists(ll_path)) {
            free(error);
            error = xstrdup("emit failed");
            write_json_row(jsonl, t->name, t->source, t->options_joined,
                           llvm_ok, liric_ok, lli_ok, liric_match, lli_match,
                           liric_rc_match, lli_rc_match,
                           llvm_rc, liric_rc, lli_rc, error);
            UPDATE_STATS_AND_PROGRESS();
            free_cmd_result(&emit_r);
            free(emit_argv);
            free(ll_path);
            free(bin_path);
            free(error);
            continue;
        }
        free_cmd_result(&emit_r);
        free(emit_argv);

        compile_argv = make_argv(4 + t->extra_args.n + 3);
        compile_argv[0] = (char *)cfg.lfortran;
        compile_argv[1] = "--no-color";
        for (j = 0; j < t->extra_args.n; j++) compile_argv[2 + j] = t->extra_args.items[j];
        compile_argv[2 + t->extra_args.n] = t->source;
        compile_argv[3 + t->extra_args.n] = "-o";
        compile_argv[4 + t->extra_args.n] = bin_path;
        compile_argv[5 + t->extra_args.n] = NULL;

        compile_r = run_cmd(compile_argv, cfg.timeout_sec, NULL, NULL, work_dir);
        free(compile_argv);
        if (compile_r.rc != 0) {
            free(error);
            error = xstrdup("native compile failed");
            write_json_row(jsonl, t->name, t->source, t->options_joined,
                           llvm_ok, liric_ok, lli_ok, liric_match, lli_match,
                           liric_rc_match, lli_rc_match,
                           llvm_rc, liric_rc, lli_rc, error);
            UPDATE_STATS_AND_PROGRESS();
            free_cmd_result(&compile_r);
            free(ll_path);
            free(bin_path);
            free(error);
            continue;
        }
        free_cmd_result(&compile_r);

        {
            char *runv[2];
            runv[0] = bin_path;
            runv[1] = NULL;
            run_argv = runv;
            run_r = run_cmd((char *const *)run_argv, cfg.timeout_sec, NULL, NULL, work_dir);
        }
        llvm_rc = run_r.rc;
        if (run_r.rc >= 0) {
            llvm_ok = 1;
            n_llvm_out = normalize_output(run_r.stdout_text);
        } else {
            free(error);
            error = xstrdup("native run failed");
        }

        {
            char *jitv[9];
            jitv[0] = (char *)cfg.probe_runner;
            jitv[1] = "--sig";
            jitv[2] = "i32_argc_argv";
            jitv[3] = "--load-lib";
            jitv[4] = (char *)cfg.runtime_lib;
            jitv[5] = ll_path;
            jitv[6] = NULL;
            jit_argv = jitv;
            jit_r = run_cmd((char *const *)jit_argv, cfg.timeout_sec, NULL, NULL, work_dir);
        }
        liric_rc = jit_r.rc;
        if (jit_r.rc >= 0 && llvm_ok) {
            liric_ok = 1;
            n_liric_out = normalize_output(jit_r.stdout_text);
            liric_match = (strcmp(n_liric_out, n_llvm_out) == 0);
            liric_rc_match = (jit_r.rc == llvm_rc);
        }

        {
            char *lliv[6];
            lliv[0] = (char *)cfg.lli;
            lliv[1] = "-O0";
            lliv[2] = "--dlopen";
            lliv[3] = (char *)cfg.runtime_lib;
            lliv[4] = ll_path;
            lliv[5] = NULL;
            lli_argv = lliv;
            lli_r = run_cmd((char *const *)lli_argv, cfg.timeout_sec, NULL, runtime_dir, work_dir);
        }
        lli_rc = lli_r.rc;
        if (lli_r.rc >= 0 && llvm_ok) {
            lli_ok = 1;
            n_lli_out = normalize_output(lli_r.stdout_text);
            lli_match = (strcmp(n_lli_out, n_llvm_out) == 0);
            lli_rc_match = (lli_r.rc == llvm_rc);
        }

        write_json_row(jsonl, t->name, t->source, t->options_joined,
                       llvm_ok, liric_ok, lli_ok, liric_match, lli_match,
                       liric_rc_match, lli_rc_match,
                       llvm_rc, liric_rc, lli_rc, error);
        UPDATE_STATS_AND_PROGRESS();

        if (liric_match) {
            if (compat_api_n == compat_api_cap) {
                size_t ncap = compat_api_cap ? compat_api_cap * 2 : 64;
                char **narr = (char **)realloc(compat_api, ncap * sizeof(char *));
                if (!narr) die("out of memory");
                compat_api = narr;
                compat_api_cap = ncap;
            }
            compat_api[compat_api_n++] = xstrdup(t->name);

            if (opts_api_n == opts_api_cap) {
                size_t ncap = opts_api_cap ? opts_api_cap * 2 : 64;
                name_opt_t *narr = (name_opt_t *)realloc(opts_api, ncap * sizeof(name_opt_t));
                if (!narr) die("out of memory");
                opts_api = narr;
                opts_api_cap = ncap;
            }
            opts_api[opts_api_n].name = xstrdup(t->name);
            opts_api[opts_api_n].options = xstrdup(t->options_joined ? t->options_joined : "");
            {
                const char *src = t->source ? t->source : "";
                const char *slash = strrchr(src, '/');
                opts_api[opts_api_n].source = xstrdup(slash ? slash + 1 : src);
            }
            opts_api_n++;
        }

        if (liric_match && lli_match) {
            if (compat_ll_n == compat_ll_cap) {
                size_t ncap = compat_ll_cap ? compat_ll_cap * 2 : 64;
                char **narr = (char **)realloc(compat_ll, ncap * sizeof(char *));
                if (!narr) die("out of memory");
                compat_ll = narr;
                compat_ll_cap = ncap;
            }
            compat_ll[compat_ll_n++] = xstrdup(t->name);

            if (opts_ll_n == opts_ll_cap) {
                size_t ncap = opts_ll_cap ? opts_ll_cap * 2 : 64;
                name_opt_t *narr = (name_opt_t *)realloc(opts_ll, ncap * sizeof(name_opt_t));
                if (!narr) die("out of memory");
                opts_ll = narr;
                opts_ll_cap = ncap;
            }
            opts_ll[opts_ll_n].name = xstrdup(t->name);
            opts_ll[opts_ll_n].options = xstrdup(t->options_joined ? t->options_joined : "");
            {
                const char *src = t->source ? t->source : "";
                const char *slash = strrchr(src, '/');
                opts_ll[opts_ll_n].source = xstrdup(slash ? slash + 1 : src);
            }
            opts_ll_n++;
        }

        free_cmd_result(&run_r);
        free_cmd_result(&jit_r);
        free_cmd_result(&lli_r);
        free(n_llvm_out);
        free(n_liric_out);
        free(n_lli_out);
        free(error);
        free(ll_path);
        free(bin_path);
    }

    fclose(jsonl);

    qsort(compat_api, compat_api_n, sizeof(char *), cmp_strptr);
    qsort(compat_ll, compat_ll_n, sizeof(char *), cmp_strptr);
    qsort(opts_api, opts_api_n, sizeof(name_opt_t), cmp_name_opt);
    qsort(opts_ll, opts_ll_n, sizeof(name_opt_t), cmp_name_opt);

    {
        FILE *f = fopen(compat_api_path, "w");
        for (i = 0; i < compat_api_n; i++) fprintf(f, "%s\n", compat_api[i]);
        fclose(f);
    }
    {
        FILE *f = fopen(compat_ll_path, "w");
        for (i = 0; i < compat_ll_n; i++) fprintf(f, "%s\n", compat_ll[i]);
        fclose(f);
    }
    {
        FILE *f = fopen(opts_api_path, "w");
        for (i = 0; i < opts_api_n; i++) {
            char *en = json_escape(opts_api[i].name);
            char *eo = json_escape(opts_api[i].options);
            char *es = json_escape(opts_api[i].source ? opts_api[i].source : "");
            fprintf(f, "{\"name\":\"%s\",\"options\":\"%s\",\"source\":\"%s\"}\n", en, eo, es);
            free(en);
            free(eo);
            free(es);
        }
        fclose(f);
    }
    {
        FILE *f = fopen(opts_ll_path, "w");
        for (i = 0; i < opts_ll_n; i++) {
            char *en = json_escape(opts_ll[i].name);
            char *eo = json_escape(opts_ll[i].options);
            char *es = json_escape(opts_ll[i].source ? opts_ll[i].source : "");
            fprintf(f, "{\"name\":\"%s\",\"options\":\"%s\",\"source\":\"%s\"}\n", en, eo, es);
            free(en);
            free(eo);
            free(es);
        }
        fclose(f);
    }
    {
        size_t frozen_n = (size_t)cfg.freeze_api_n;
        if (frozen_n > compat_api_n) frozen_n = compat_api_n;

        {
            FILE *f = fopen(frozen_api_path, "w");
            if (!f) die("failed to open %s", frozen_api_path);
            for (i = 0; i < frozen_n; i++) fprintf(f, "%s\n", compat_api[i]);
            fclose(f);
        }

        {
            FILE *f = fopen(frozen_opts_path, "w");
            if (!f) die("failed to open %s", frozen_opts_path);
            for (i = 0; i < frozen_n; i++) {
                const char *name = compat_api[i];
                const name_opt_t *entry = find_option_by_name(opts_api, opts_api_n, name);
                const char *opts = entry ? entry->options : "";
                const char *src = entry ? entry->source : "";
                char *en = json_escape(name);
                char *eo = json_escape(opts);
                char *es = json_escape(src);
                fprintf(f, "{\"name\":\"%s\",\"options\":\"%s\",\"source\":\"%s\"}\n", en, eo, es);
                free(en);
                free(eo);
                free(es);
            }
            fclose(f);
        }
    }

    printf("\nResults written to %s\n", jsonl_path);
    printf("processed:  %zu/%zu\n", processed, eligible);
    printf("llvm_ok:    %zu (%.1f%%)\n", llvm_ok_count, processed ? (100.0 * (double)llvm_ok_count / (double)processed) : 0.0);
    printf("liric_match:%zu (%.1f%%)\n", liric_match_count, processed ? (100.0 * (double)liric_match_count / (double)processed) : 0.0);
    printf("lli_match:  %zu (%.1f%%)\n", lli_match_count, processed ? (100.0 * (double)lli_match_count / (double)processed) : 0.0);
    printf("both_match: %zu (%.1f%%)\n", both_match_count, processed ? (100.0 * (double)both_match_count / (double)processed) : 0.0);
    printf("compat_api: %zu tests -> %s\n", compat_api_n, compat_api_path);
    printf("compat_ll:  %zu tests -> %s\n", compat_ll_n, compat_ll_path);
    printf("compat_api_frozen: %d requested, %zu written -> %s\n",
           cfg.freeze_api_n,
           ((size_t)cfg.freeze_api_n > compat_api_n) ? compat_api_n : (size_t)cfg.freeze_api_n,
           frozen_api_path);

#undef UPDATE_STATS_AND_PROGRESS

    for (i = 0; i < compat_api_n; i++) free(compat_api[i]);
    for (i = 0; i < compat_ll_n; i++) free(compat_ll[i]);
    for (i = 0; i < opts_api_n; i++) {
        free(opts_api[i].name);
        free(opts_api[i].options);
        free(opts_api[i].source);
    }
    for (i = 0; i < opts_ll_n; i++) {
        free(opts_ll[i].name);
        free(opts_ll[i].options);
        free(opts_ll[i].source);
    }

    free(compat_api);
    free(compat_ll);
    free(opts_api);
    free(opts_ll);
    free(runtime_dir);
    free(ll_dir);
    free(bin_dir);
    free(jsonl_path);
    free(compat_api_path);
    free(compat_ll_path);
    free(opts_api_path);
    free(opts_ll_path);
    free(frozen_api_path);
    free(frozen_opts_path);
    free_testlist(&tests);
    return 0;
}
