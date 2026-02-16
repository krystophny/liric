/*
 * bench_tcc: TinyCC baseline vs liric public compiler API.
 *
 * Metrics:
 *   - WALL-CLOCK: subprocess `tcc -o exe file.c` vs `liric_probe_runner --no-exec`
 *   - IN-PROCESS: `tcc_compile_string()+tcc_relocate()` vs `lr_compiler_feed_ll()+lookup`
 *
 * Usage: ./build/bench_tcc [--iters N] [--bench-dir PATH] [--work-dir PATH] [--policy direct|ir]
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

extern char **environ;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

typedef struct {
    const char *name;
    const char *c_src;
    const char *ll_src;
    int expected_rc;
} bench_case_t;

static const bench_case_t g_cases[] = {
    {
        .name = "ret42",
        .c_src = "int main(void) { return 42; }\n",
        .ll_src =
            "define i32 @main() {\n"
            "entry:\n"
            "  ret i32 42\n"
            "}\n",
        .expected_rc = 42,
    },
    {
        .name = "add",
        .c_src =
            "int add(int a, int b) { return a + b; }\n"
            "int main(void) { return add(10, 32); }\n",
        .ll_src =
            "define i32 @add(i32 %a, i32 %b) {\n"
            "entry:\n"
            "  %c = add i32 %a, %b\n"
            "  ret i32 %c\n"
            "}\n"
            "define i32 @main() {\n"
            "entry:\n"
            "  %r = call i32 @add(i32 10, i32 32)\n"
            "  ret i32 %r\n"
            "}\n",
        .expected_rc = 42,
    },
    {
        .name = "arith_chain",
        .c_src =
            "int arith(int a, int b) {\n"
            "  int sum = a + b;\n"
            "  int prod = sum * b;\n"
            "  int diff = prod - a;\n"
            "  return diff;\n"
            "}\n"
            "int main(void) { return arith(3, 4); }\n",
        .ll_src =
            "define i32 @arith(i32 %a, i32 %b) {\n"
            "entry:\n"
            "  %sum = add i32 %a, %b\n"
            "  %prod = mul i32 %sum, %b\n"
            "  %diff = sub i32 %prod, %a\n"
            "  ret i32 %diff\n"
            "}\n"
            "define i32 @main() {\n"
            "entry:\n"
            "  %r = call i32 @arith(i32 3, i32 4)\n"
            "  ret i32 %r\n"
            "}\n",
        .expected_rc = 25,
    },
    {
        .name = "loop_sum",
        .c_src =
            "int sum_to(int n) {\n"
            "  int s = 0;\n"
            "  for (int i = 1; i <= n; i++) s += i;\n"
            "  return s;\n"
            "}\n"
            "int main(void) { return sum_to(10); }\n",
        .ll_src =
            "define i32 @sum_to(i32 %n) {\n"
            "entry:\n"
            "  br label %loop\n"
            "loop:\n"
            "  %i = phi i32 [1, %entry], [%i_next, %loop]\n"
            "  %s = phi i32 [0, %entry], [%s_next, %loop]\n"
            "  %s_next = add i32 %s, %i\n"
            "  %i_next = add i32 %i, 1\n"
            "  %cmp = icmp sle i32 %i_next, %n\n"
            "  br i1 %cmp, label %loop, label %done\n"
            "done:\n"
            "  ret i32 %s_next\n"
            "}\n"
            "define i32 @main() {\n"
            "entry:\n"
            "  %r = call i32 @sum_to(i32 10)\n"
            "  ret i32 %r\n"
            "}\n",
        .expected_rc = 55,
    },
    {
        .name = "fib20",
        .c_src =
            "int fib(int n) {\n"
            "  if (n <= 1) return n;\n"
            "  return fib(n-1) + fib(n-2);\n"
            "}\n"
            "int main(void) { return fib(20) % 256; }\n",
        .ll_src =
            "define i32 @fib(i32 %n) {\n"
            "entry:\n"
            "  %cmp = icmp sle i32 %n, 1\n"
            "  br i1 %cmp, label %base, label %rec\n"
            "base:\n"
            "  ret i32 %n\n"
            "rec:\n"
            "  %n1 = sub i32 %n, 1\n"
            "  %f1 = call i32 @fib(i32 %n1)\n"
            "  %n2 = sub i32 %n, 2\n"
            "  %f2 = call i32 @fib(i32 %n2)\n"
            "  %r = add i32 %f1, %f2\n"
            "  ret i32 %r\n"
            "}\n"
            "define i32 @main() {\n"
            "entry:\n"
            "  %r = call i32 @fib(i32 20)\n"
            "  %rc = srem i32 %r, 256\n"
            "  ret i32 %rc\n"
            "}\n",
        .expected_rc = 109,
    },
};

#define NUM_CASES (sizeof(g_cases) / sizeof(g_cases[0]))

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(data, f);
    fclose(f);
    return 0;
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

static double run_exec_timed_expected(char *const argv[], int expected_rc) {
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

    if (WIFEXITED(status) && WEXITSTATUS(status) == expected_rc)
        return t1 - t0;
    return -1.0;
}

static int verify_exe(const char *path, int expected_rc) {
    pid_t pid;
    char *argv[] = {(char *)path, NULL};
    int err = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    int status;
    if (err != 0) return -1;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == expected_rc ? 0 : -1;
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

int main(int argc, char **argv) {
    int iters = 5;
    const char *bench_dir = "/tmp/liric_bench";
    const char *work_dir = "/tmp/bench_tcc";
    const char *policy = "direct";
    const char *build_dir;
    char probe_runner[PATH_MAX];
    char liric_mode_name[32] = "isel";
    char tcc_bin[PATH_MAX] = "tcc";
    lr_backend_t backend = LR_BACKEND_ISEL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--work-dir") == 0 && i + 1 < argc) {
            work_dir = argv[++i];
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            policy = argv[++i];
        } else {
            fprintf(stderr, "usage: bench_lane_micro [--iters N] [--bench-dir PATH] [--work-dir PATH] [--policy direct|ir]\n");
            return 1;
        }
    }
    if (iters < 1) iters = 1;
    if (strcmp(policy, "direct") != 0 && strcmp(policy, "ir") != 0) {
        fprintf(stderr, "error: invalid --policy value: %s (expected direct|ir)\n", policy);
        return 1;
    }

    if (mkdir_p(work_dir) != 0) {
        fprintf(stderr, "error: failed to create work dir: %s\n", work_dir);
        return 1;
    }
    if (mkdir_p(bench_dir) != 0) {
        fprintf(stderr, "error: failed to create bench dir: %s\n", bench_dir);
        return 1;
    }

    build_dir = getenv("LIRIC_BUILD_DIR");
    if (!build_dir) build_dir = "build";
    snprintf(probe_runner, sizeof(probe_runner), "%s/liric_probe_runner", build_dir);
    if (access(probe_runner, X_OK) != 0) {
        fprintf(stderr, "error: %s not found (set LIRIC_BUILD_DIR)\n", probe_runner);
        return 1;
    }

    if (backend_from_env(&backend) != 0) {
        fprintf(stderr, "error: invalid LIRIC_COMPILE_MODE value\n");
        return 1;
    }
    {
        const char *m = getenv("LIRIC_COMPILE_MODE");
        if (m && m[0]) {
            snprintf(liric_mode_name, sizeof(liric_mode_name), "%s", m);
        }
    }

    if (access("/usr/bin/tcc", X_OK) == 0)
        snprintf(tcc_bin, sizeof(tcc_bin), "%s", "/usr/bin/tcc");
    else if (!executable_in_path("tcc")) {
        fprintf(stderr, "error: tcc not found in PATH\n");
        return 1;
    }

    printf("bench_tcc: %d cases, %d iterations (best-of), mode=%s, policy=%s\n\n",
           (int)NUM_CASES, iters, liric_mode_name, policy);

    printf("=== WALL-CLOCK: subprocess (tcc compile) vs (liric session compile) ===\n");
    printf("%-16s %10s %10s %8s %s\n", "test", "tcc(us)", "liric(us)", "ratio", "status");
    printf("%-16s %10s %10s %8s %s\n", "----", "-------", "--------", "-----", "------");

    double total_tcc = 0.0, total_liric = 0.0;
    int wall_passed = 0;

    for (size_t ci = 0; ci < NUM_CASES; ci++) {
        const bench_case_t *tc = &g_cases[ci];
        char c_path[PATH_MAX], ll_path[PATH_MAX];
        char out_tcc[PATH_MAX];
        double best_tcc = 1e18, best_liric = 1e18;

        snprintf(c_path, sizeof(c_path), "%s/%s.c", work_dir, tc->name);
        snprintf(ll_path, sizeof(ll_path), "%s/%s.ll", work_dir, tc->name);
        snprintf(out_tcc, sizeof(out_tcc), "%s/out_tcc_%s", work_dir, tc->name);

        if (write_file(c_path, tc->c_src) != 0 || write_file(ll_path, tc->ll_src) != 0) {
            fprintf(stderr, "error: failed to write test input files for %s\n", tc->name);
            return 1;
        }

        for (int it = 0; it < iters; it++) {
            char *tcc_argv[] = {tcc_bin, "-o", out_tcc, c_path, NULL};
            double t = run_exec_timed_expected(tcc_argv, 0);
            if (t >= 0.0 && t < best_tcc) best_tcc = t;
        }

        for (int it = 0; it < iters; it++) {
            char *liric_argv[] = {
                probe_runner,
                "--no-exec",
                "--policy",
                (char *)policy,
                "--func",
                "main",
                "--sig",
                "i32",
                ll_path,
                NULL
            };
            double t = run_exec_timed_expected(liric_argv, 0);
            if (t >= 0.0 && t < best_liric) best_liric = t;
        }

        {
            int tcc_ok = verify_exe(out_tcc, tc->expected_rc);
            const char *status = "";
            if (best_tcc >= 1e17 || best_liric >= 1e17 || tcc_ok != 0) {
                status = (tcc_ok != 0) ? "tcc FAIL" : "liric FAIL";
            } else {
                status = "OK";
                wall_passed++;
            }

            if (best_tcc < 1e17) total_tcc += best_tcc;
            if (best_liric < 1e17) total_liric += best_liric;

            {
                double ratio = (best_liric > 0 && best_tcc > 0 &&
                                best_tcc < 1e17 && best_liric < 1e17)
                                   ? best_tcc / best_liric
                                   : 0.0;
                printf("%-16s %10.0f %10.0f %7.2fx  %s\n",
                       tc->name,
                       best_tcc < 1e17 ? best_tcc : 0.0,
                       best_liric < 1e17 ? best_liric : 0.0,
                       ratio,
                       status);
            }
        }
    }

    {
        double ratio = (total_liric > 0.0) ? total_tcc / total_liric : 0.0;
        printf("%-16s %10s %10s %8s\n", "----", "-------", "--------", "-----");
        printf("%-16s %10.0f %10.0f %7.2fx\n", "TOTAL", total_tcc, total_liric, ratio);
    }

    printf("\n=== IN-PROCESS: tcc (libtcc) vs liric public compiler API ===\n");
    printf("%-16s %10s %10s %10s | %10s %10s %10s | %7s\n",
           "test", "tcc:comp", "tcc:reloc", "tcc:total",
           "lr:feed", "lr:lookup", "lr:total", "ratio");
    printf("%-16s %10s %10s %10s | %10s %10s %10s | %7s\n",
           "----", "--------", "---------", "---------",
           "--------", "---------", "--------", "-----");

    double ip_tcc_total = 0.0;
    double ip_lr_feed_total = 0.0, ip_lr_lookup_total = 0.0;
    int inproc_passed = 0;

    for (size_t ci = 0; ci < NUM_CASES; ci++) {
        const bench_case_t *tc = &g_cases[ci];
        size_t ll_len = strlen(tc->ll_src);
        double best_tcc_comp = 1e18, best_tcc_reloc = 1e18;
        double best_lr_feed = 1e18, best_lr_lookup = 1e18;
        int case_ok = 1;

        for (int it = 0; it < iters; it++) {
            TCCState *s = tcc_new();
            if (!s) {
                case_ok = 0;
                break;
            }
            tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

            double t0 = now_us();
            if (tcc_compile_string(s, tc->c_src) != 0) {
                tcc_delete(s);
                case_ok = 0;
                break;
            }
            double t1 = now_us();
            if (tcc_relocate(s) < 0) {
                tcc_delete(s);
                case_ok = 0;
                break;
            }
            double t2 = now_us();

            if (t1 - t0 < best_tcc_comp) best_tcc_comp = t1 - t0;
            if (t2 - t1 < best_tcc_reloc) best_tcc_reloc = t2 - t1;
            tcc_delete(s);
        }

        for (int it = 0; it < iters; it++) {
            lr_compiler_config_t ccfg;
            lr_compiler_error_t cerr;
            lr_compiler_t *c;
            double t0, t1, t2, t3;
            int (*fn)(void);
            void *sym;
            int rc;

            memset(&ccfg, 0, sizeof(ccfg));
            ccfg.policy = (strcmp(policy, "ir") == 0) ? LR_POLICY_IR : LR_POLICY_DIRECT;
            ccfg.backend = backend;
            ccfg.target = NULL;

            c = lr_compiler_create(&ccfg, &cerr);
            if (!c) {
                case_ok = 0;
                break;
            }

            t0 = now_us();
            if (lr_compiler_feed_ll(c, tc->ll_src, ll_len, &cerr) != 0) {
                lr_compiler_destroy(c);
                case_ok = 0;
                break;
            }
            t1 = now_us();

            t2 = now_us();
            sym = lr_compiler_lookup(c, "main");
            t3 = now_us();
            if (!sym) {
                lr_compiler_destroy(c);
                case_ok = 0;
                break;
            }
            if (sizeof(fn) != sizeof(sym)) {
                lr_compiler_destroy(c);
                case_ok = 0;
                break;
            }
            memcpy(&fn, &sym, sizeof(fn));

            rc = fn();
            if ((rc & 0xff) != tc->expected_rc) {
                lr_compiler_destroy(c);
                case_ok = 0;
                break;
            }

            if (t1 - t0 < best_lr_feed) best_lr_feed = t1 - t0;
            if (t3 - t2 < best_lr_lookup) best_lr_lookup = t3 - t2;

            lr_compiler_destroy(c);
        }

        {
            double tcc_tot = best_tcc_comp + best_tcc_reloc;
            double lr_tot = best_lr_feed + best_lr_lookup;
            double r = (lr_tot > 0.0) ? tcc_tot / lr_tot : 0.0;
            printf("%-16s %10.1f %10.1f %10.1f | %10.1f %10.1f %10.1f | %6.1fx\n",
                   tc->name,
                   best_tcc_comp < 1e17 ? best_tcc_comp : 0.0,
                   best_tcc_reloc < 1e17 ? best_tcc_reloc : 0.0,
                   tcc_tot < 1e17 ? tcc_tot : 0.0,
                   best_lr_feed < 1e17 ? best_lr_feed : 0.0,
                   best_lr_lookup < 1e17 ? best_lr_lookup : 0.0,
                   lr_tot < 1e17 ? lr_tot : 0.0,
                   r);

            if (case_ok && tcc_tot < 1e17 && lr_tot < 1e17) {
                inproc_passed++;
            }
            if (tcc_tot < 1e17) ip_tcc_total += tcc_tot;
            if (best_lr_feed < 1e17) ip_lr_feed_total += best_lr_feed;
            if (best_lr_lookup < 1e17) ip_lr_lookup_total += best_lr_lookup;
        }
    }

    {
        double lr_tot_all = ip_lr_feed_total + ip_lr_lookup_total;
        double r_all = (lr_tot_all > 0.0) ? ip_tcc_total / lr_tot_all : 0.0;
        printf("%-16s %10s %10s %10s | %10s %10s %10s | %7s\n",
               "----", "--------", "---------", "---------",
               "--------", "---------", "--------", "-----");
        printf("%-16s %10s %10s %10.1f | %10.1f %10.1f %10.1f | %6.1fx\n",
               "TOTAL", "", "", ip_tcc_total,
               ip_lr_feed_total, ip_lr_lookup_total, lr_tot_all, r_all);

        printf("\nAll times in microseconds (us). ratio > 1 = liric faster.\n");
        printf("tcc:comp = tcc_compile_string(), tcc:reloc = tcc_relocate()\n");
        printf("lr:feed = lr_compiler_feed_ll(), lr:lookup = lr_compiler_lookup()\n");

        {
            char summary_path[PATH_MAX];
            FILE *sf;
            const char *status = (wall_passed == (int)NUM_CASES && inproc_passed == (int)NUM_CASES)
                                     ? "OK"
                                     : "FAILED";
            double wall_ratio = (total_liric > 0.0) ? total_tcc / total_liric : 0.0;
            snprintf(summary_path, sizeof(summary_path), "%s/bench_tcc_summary.json", bench_dir);
            sf = fopen(summary_path, "w");
            if (!sf) {
                fprintf(stderr, "error: failed to write summary: %s\n", summary_path);
                return 1;
            }
            fprintf(sf, "{\n");
            fprintf(sf, "  \"status\": \"%s\",\n", status);
            fprintf(sf, "  \"mode\": \"%s\",\n", liric_mode_name);
            fprintf(sf, "  \"policy\": \"%s\",\n", policy);
            fprintf(sf, "  \"iters\": %d,\n", iters);
            fprintf(sf, "  \"total_cases\": %d,\n", (int)NUM_CASES);
            fprintf(sf, "  \"wall_passed\": %d,\n", wall_passed);
            fprintf(sf, "  \"inproc_passed\": %d,\n", inproc_passed);
            fprintf(sf, "  \"wall_tcc_total_us\": %.6f,\n", total_tcc);
            fprintf(sf, "  \"wall_liric_total_us\": %.6f,\n", total_liric);
            fprintf(sf, "  \"wall_speedup_ratio\": %.6f,\n", wall_ratio);
            fprintf(sf, "  \"inproc_tcc_total_us\": %.6f,\n", ip_tcc_total);
            fprintf(sf, "  \"inproc_liric_parse_total_us\": %.6f,\n", ip_lr_feed_total);
            fprintf(sf, "  \"inproc_liric_compile_total_us\": %.6f,\n", ip_lr_lookup_total);
            fprintf(sf, "  \"inproc_liric_total_us\": %.6f,\n", lr_tot_all);
            fprintf(sf, "  \"inproc_speedup_ratio\": %.6f\n", r_all);
            fprintf(sf, "}\n");
            fclose(sf);
            printf("Summary: %s\n", summary_path);
        }
    }

    return 0;
}
