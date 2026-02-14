#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <spawn.h>

extern char **environ;

#define MAX_MODES 8

typedef struct {
    const char *name;
    const char *ll_src;
    int expected_rc;
} bench_case_t;

typedef struct {
    const char *mode;
    double liric_total_us;
    double llvm_total_us;
    int liric_failures;
    int llvm_failures;
} mode_summary_t;

static const bench_case_t g_cases[] = {
    {
        .name = "ret42",
        .ll_src =
            "define i32 @main() {\n"
            "entry:\n"
            "  ret i32 42\n"
            "}\n",
        .expected_rc = 42,
    },
    {
        .name = "add",
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

#define NUM_CASES ((int)(sizeof(g_cases) / sizeof(g_cases[0])))

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t n;
    char *p;

    if (!path || path[0] == '\0')
        return -1;

    n = strlen(path);
    if (n >= sizeof(tmp))
        return -1;

    memcpy(tmp, path, n + 1);
    if (tmp[n - 1] == '/')
        tmp[n - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
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

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(data, f);
    fclose(f);
    return 0;
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

static double run_exec_timed(char *const argv[]) {
    pid_t pid;
    int status;
    int devnull;
    int err;
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

static int verify_exe(const char *path, int expected_rc) {
    pid_t pid;
    int status;
    int err;
    char *argv[] = {(char *)path, NULL};

    err = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (err != 0)
        return -1;

    if (waitpid(pid, &status, 0) < 0)
        return -1;

    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status) == expected_rc ? 0 : -1;
}

static int is_supported_mode(const char *mode) {
    return strcmp(mode, "isel") == 0 ||
           strcmp(mode, "copy_patch") == 0 ||
           strcmp(mode, "llvm") == 0;
}

static int parse_modes(char *modes_csv, char **modes, int max_modes) {
    char *tok;
    int count = 0;

    tok = strtok(modes_csv, ",");
    while (tok) {
        if (count >= max_modes)
            return -1;
        if (!is_supported_mode(tok))
            return -1;
        modes[count++] = tok;
        tok = strtok(NULL, ",");
    }

    return count;
}

static void print_usage(void) {
    printf("usage: bench_exe_matrix [options]\n");
    printf("  --iters N            iterations per case/mode (default: 3)\n");
    printf("  --bench-dir PATH     output directory (default: /tmp/liric_bench)\n");
    printf("  --build-dir PATH     build dir for liric binary (default: build)\n");
    printf("  --liric PATH         liric executable path (default: <build-dir>/liric)\n");
    printf("  --llvm-driver PATH   LLVM driver for ll->exe baseline (default: clang)\n");
    printf("  --modes CSV          compile modes (default: isel,copy_patch,llvm)\n");
    printf("  --json PATH          summary json path (default: <bench-dir>/bench_exe_matrix_summary.json)\n");
}

int main(int argc, char **argv) {
    int iters = 3;
    int any_fail = 0;
    int i;

    const char *bench_dir = "/tmp/liric_bench";
    const char *build_dir = "build";
    const char *liric_arg = NULL;
    const char *llvm_driver = "clang";
    const char *json_path_arg = NULL;
    char modes_csv[128] = "isel,copy_patch,llvm";
    char modes_desc[128] = "isel,copy_patch,llvm";

    char liric_path[PATH_MAX];
    char json_path[PATH_MAX];

    char *modes[MAX_MODES];
    int mode_count;
    mode_summary_t summaries[MAX_MODES];

    const char *old_mode = getenv("LIRIC_COMPILE_MODE");
    char *old_mode_copy = NULL;

    if (old_mode)
        old_mode_copy = strdup(old_mode);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bench-dir") == 0 && i + 1 < argc) {
            bench_dir = argv[++i];
        } else if (strcmp(argv[i], "--build-dir") == 0 && i + 1 < argc) {
            build_dir = argv[++i];
        } else if (strcmp(argv[i], "--liric") == 0 && i + 1 < argc) {
            liric_arg = argv[++i];
        } else if (strcmp(argv[i], "--llvm-driver") == 0 && i + 1 < argc) {
            llvm_driver = argv[++i];
        } else if (strcmp(argv[i], "--modes") == 0 && i + 1 < argc) {
            snprintf(modes_csv, sizeof(modes_csv), "%s", argv[++i]);
            snprintf(modes_desc, sizeof(modes_desc), "%s", modes_csv);
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path_arg = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            free(old_mode_copy);
            return 0;
        } else {
            print_usage();
            free(old_mode_copy);
            return 1;
        }
    }

    if (iters < 1)
        iters = 1;

    if (liric_arg) {
        snprintf(liric_path, sizeof(liric_path), "%s", liric_arg);
    } else {
        snprintf(liric_path, sizeof(liric_path), "%s/liric", build_dir);
    }

    if (json_path_arg) {
        snprintf(json_path, sizeof(json_path), "%s", json_path_arg);
    } else {
        snprintf(json_path, sizeof(json_path), "%s/bench_exe_matrix_summary.json", bench_dir);
    }

    mode_count = parse_modes(modes_csv, modes, MAX_MODES);
    if (mode_count <= 0) {
        fprintf(stderr, "error: invalid --modes value\n");
        free(old_mode_copy);
        return 1;
    }

    if (access(liric_path, X_OK) != 0) {
        fprintf(stderr, "error: liric executable not found: %s\n", liric_path);
        free(old_mode_copy);
        return 1;
    }

    if (!executable_in_path(llvm_driver)) {
        fprintf(stderr, "error: llvm driver not found: %s\n", llvm_driver);
        free(old_mode_copy);
        return 1;
    }

    if (mkdir_p(bench_dir) != 0) {
        fprintf(stderr, "error: failed to create bench dir: %s\n", bench_dir);
        free(old_mode_copy);
        return 1;
    }

    printf("bench_exe_matrix: %d cases, %d iterations (best-of), modes=%s\n\n",
           NUM_CASES, iters, modes_desc);

    for (i = 0; i < mode_count; i++) {
        const char *mode = modes[i];
        int ci;
        char mode_dir[PATH_MAX];

        double liric_total = 0.0;
        double llvm_total = 0.0;
        int liric_failures = 0;
        int llvm_failures = 0;

        snprintf(mode_dir, sizeof(mode_dir), "%s/%s", bench_dir, mode);
        if (mkdir_p(mode_dir) != 0) {
            fprintf(stderr, "error: failed to create mode dir: %s\n", mode_dir);
            free(old_mode_copy);
            return 1;
        }

        printf("=== MODE: %s (liric ll->exe vs llvm ll->exe) ===\n", mode);
        printf("%-16s %12s %12s %8s %s\n", "test", "llvm(us)", "liric(us)", "ratio", "status");
        printf("%-16s %12s %12s %8s %s\n", "----", "--------", "---------", "-----", "------");

        for (ci = 0; ci < NUM_CASES; ci++) {
            const bench_case_t *tc = &g_cases[ci];
            char ll_path[PATH_MAX];
            char liric_out[PATH_MAX];
            char llvm_out[PATH_MAX];
            double best_liric = 1e18;
            double best_llvm = 1e18;
            int it;
            int liric_ok;
            int llvm_ok;
            const char *status = "OK";
            double ratio;

            snprintf(ll_path, sizeof(ll_path), "%s/%s.ll", mode_dir, tc->name);
            snprintf(liric_out, sizeof(liric_out), "%s/liric_%s", mode_dir, tc->name);
            snprintf(llvm_out, sizeof(llvm_out), "%s/llvm_%s", mode_dir, tc->name);

            if (write_file(ll_path, tc->ll_src) != 0) {
                fprintf(stderr, "error: failed to write %s\n", ll_path);
                free(old_mode_copy);
                return 1;
            }

            for (it = 0; it < iters; it++) {
                double t;
                char *liric_argv[] = { (char *)liric_path, "-o", liric_out, ll_path, NULL };
                setenv("LIRIC_COMPILE_MODE", mode, 1);
                t = run_exec_timed(liric_argv);
                if (t >= 0.0 && t < best_liric)
                    best_liric = t;
            }

            for (it = 0; it < iters; it++) {
                double t;
                char *llvm_argv[] = {
                    (char *)llvm_driver,
                    "-O0",
                    "-Wno-override-module",
                    "-x",
                    "ir",
                    "-o",
                    llvm_out,
                    ll_path,
                    NULL
                };
                t = run_exec_timed(llvm_argv);
                if (t >= 0.0 && t < best_llvm)
                    best_llvm = t;
            }

            liric_ok = verify_exe(liric_out, tc->expected_rc);
            llvm_ok = verify_exe(llvm_out, tc->expected_rc);

            if (best_liric >= 1e17 || liric_ok != 0) {
                liric_failures++;
                status = "liric FAIL";
            }
            if (best_llvm >= 1e17 || llvm_ok != 0) {
                llvm_failures++;
                status = (strcmp(status, "OK") == 0) ? "llvm FAIL" : "BOTH FAIL";
            }

            if (best_liric < 1e17)
                liric_total += best_liric;
            if (best_llvm < 1e17)
                llvm_total += best_llvm;

            ratio = (best_liric > 0.0 && best_liric < 1e17 && best_llvm < 1e17)
                        ? best_llvm / best_liric
                        : 0.0;

            printf("%-16s %12.0f %12.0f %7.2fx  %s\n",
                   tc->name,
                   (best_llvm < 1e17 ? best_llvm : 0.0),
                   (best_liric < 1e17 ? best_liric : 0.0),
                   ratio,
                   status);
        }

        printf("%-16s %12s %12s %8s\n", "----", "--------", "---------", "-----");
        printf("%-16s %12.0f %12.0f %7.2fx\n\n", "TOTAL", llvm_total, liric_total,
               (liric_total > 0.0 ? llvm_total / liric_total : 0.0));

        summaries[i].mode = mode;
        summaries[i].liric_total_us = liric_total;
        summaries[i].llvm_total_us = llvm_total;
        summaries[i].liric_failures = liric_failures;
        summaries[i].llvm_failures = llvm_failures;

        if (liric_failures != 0 || llvm_failures != 0)
            any_fail = 1;
    }

    {
        FILE *jf = fopen(json_path, "w");
        if (!jf) {
            fprintf(stderr, "error: failed to write summary json: %s\n", json_path);
            free(old_mode_copy);
            return 1;
        }

        fprintf(jf, "{\n");
        fprintf(jf, "  \"bench\": \"bench_exe_matrix\",\n");
        fprintf(jf, "  \"iters\": %d,\n", iters);
        fprintf(jf, "  \"cases_total\": %d,\n", NUM_CASES);
        fprintf(jf, "  \"bench_dir\": \"%s\",\n", bench_dir);
        fprintf(jf, "  \"liric\": \"%s\",\n", liric_path);
        fprintf(jf, "  \"llvm_driver\": \"%s\",\n", llvm_driver);
        fprintf(jf, "  \"modes\": [\n");

        for (i = 0; i < mode_count; i++) {
            double ratio = summaries[i].liric_total_us > 0.0
                               ? summaries[i].llvm_total_us / summaries[i].liric_total_us
                               : 0.0;
            fprintf(jf,
                    "    {\"mode\":\"%s\",\"llvm_total_us\":%.0f,\"liric_total_us\":%.0f,\"ratio\":%.6f,\"llvm_failures\":%d,\"liric_failures\":%d}%s\n",
                    summaries[i].mode,
                    summaries[i].llvm_total_us,
                    summaries[i].liric_total_us,
                    ratio,
                    summaries[i].llvm_failures,
                    summaries[i].liric_failures,
                    (i + 1 == mode_count) ? "" : ",");
        }

        fprintf(jf, "  ]\n");
        fprintf(jf, "}\n");
        fclose(jf);
    }

    if (old_mode_copy) {
        setenv("LIRIC_COMPILE_MODE", old_mode_copy, 1);
        free(old_mode_copy);
    } else {
        unsetenv("LIRIC_COMPILE_MODE");
    }

    printf("summary: %s\n", json_path);

    if (any_fail) {
        fprintf(stderr, "bench_exe_matrix: failures detected\n");
        return 1;
    }

    return 0;
}
