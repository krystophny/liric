/*
 * bench_tcc: Compare liric exe-mode compile speed against TinyCC.
 *
 * Metrics:
 *   - WALL-CLOCK: fork/exec `tcc -o exe file.c` vs `liric -o exe file.ll`
 *   - IN-PROCESS: liric lr_parse_ll() + lr_jit_add_module() with parse/compile split
 *
 * Usage: ./build/bench_tcc [--iters N]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <libtcc.h>
#include <liric/liric_legacy.h>

#define BENCH_DIR "/tmp/bench_tcc"

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
        .expected_rc = 109,  /* fib(20)=6765, 6765%256=109 */
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

extern char **environ;

static double run_exec_timed(char *const argv[]) {
    pid_t pid;
    int devnull = open("/dev/null", O_WRONLY);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, devnull, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, devnull, STDERR_FILENO);

    double t0 = now_us();
    int err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    if (err != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(devnull);
        return -1.0;
    }
    int status;
    waitpid(pid, &status, 0);
    double t1 = now_us();

    posix_spawn_file_actions_destroy(&fa);
    close(devnull);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return t1 - t0;
    return -1.0;
}

static int verify_exe(const char *path, int expected_rc) {
    pid_t pid;
    char *argv[] = { (char *)path, NULL };
    int err = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    if (err != 0) return -1;
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == expected_rc ? 0 : -1;
}

int main(int argc, char **argv) {
    int iters = 5;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            iters = atoi(argv[++i]);
    }
    if (iters < 1) iters = 1;

    mkdir(BENCH_DIR, 0755);

    char liric_bin[512] = {0};
    const char *dir = getenv("LIRIC_BUILD_DIR");
    if (!dir) dir = "build";
    snprintf(liric_bin, sizeof(liric_bin), "%s/liric", dir);

    if (access(liric_bin, X_OK) != 0) {
        fprintf(stderr, "error: %s not found (set LIRIC_BUILD_DIR)\n", liric_bin);
        return 1;
    }

    char tcc_bin[512] = "tcc";
    if (access("/usr/bin/tcc", X_OK) == 0)
        strcpy(tcc_bin, "/usr/bin/tcc");
    else if (system("tcc --version >/dev/null 2>&1") != 0) {
        fprintf(stderr, "error: tcc not found in PATH\n");
        return 1;
    }

    /* ---- Wall-clock: subprocess exe-mode ---- */
    printf("bench_tcc: %d cases, %d iterations (best-of)\n\n", (int)NUM_CASES, iters);
    printf("=== WALL-CLOCK: subprocess exe-mode (tcc C->exe vs liric ll->exe) ===\n");
    printf("%-16s %10s %10s %8s %s\n", "test", "tcc(us)", "liric(us)", "ratio", "status");
    printf("%-16s %10s %10s %8s %s\n", "----", "-------", "--------", "-----", "------");

    double total_tcc = 0, total_liric = 0;

    for (size_t ci = 0; ci < NUM_CASES; ci++) {
        const bench_case_t *tc = &g_cases[ci];

        char c_path[256], ll_path[256];
        snprintf(c_path, sizeof(c_path), BENCH_DIR "/%s.c", tc->name);
        snprintf(ll_path, sizeof(ll_path), BENCH_DIR "/%s.ll", tc->name);
        write_file(c_path, tc->c_src);
        write_file(ll_path, tc->ll_src);

        char out_tcc[256], out_liric[256];
        snprintf(out_tcc, sizeof(out_tcc), BENCH_DIR "/out_tcc_%s", tc->name);
        snprintf(out_liric, sizeof(out_liric), BENCH_DIR "/out_liric_%s", tc->name);

        char *tcc_argv[] = { tcc_bin, "-o", out_tcc, c_path, NULL };
        char *liric_argv[] = { liric_bin, "-o", out_liric, ll_path, NULL };

        double best_tcc = 1e18, best_liric = 1e18;

        for (int it = 0; it < iters; it++) {
            double t = run_exec_timed(tcc_argv);
            if (t >= 0 && t < best_tcc) best_tcc = t;
        }
        for (int it = 0; it < iters; it++) {
            double t = run_exec_timed(liric_argv);
            if (t >= 0 && t < best_liric) best_liric = t;
        }

        int tcc_ok = verify_exe(out_tcc, tc->expected_rc);
        int liric_ok = verify_exe(out_liric, tc->expected_rc);

        const char *status = "";
        if (tcc_ok != 0 && liric_ok != 0) status = "BOTH FAIL";
        else if (tcc_ok != 0) status = "tcc FAIL";
        else if (liric_ok != 0) status = "liric FAIL";
        else status = "OK";

        double ratio = (best_liric > 0 && best_tcc > 0) ? best_tcc / best_liric : 0;
        printf("%-16s %10.0f %10.0f %7.2fx  %s\n",
               tc->name, best_tcc, best_liric, ratio, status);

        if (best_tcc < 1e17) total_tcc += best_tcc;
        if (best_liric < 1e17) total_liric += best_liric;
    }

    printf("%-16s %10s %10s %8s\n", "----", "-------", "--------", "-----");
    double ratio = (total_liric > 0) ? total_tcc / total_liric : 0;
    printf("%-16s %10.0f %10.0f %7.2fx\n", "TOTAL", total_tcc, total_liric, ratio);

    /* ---- In-process: TCC (libtcc) vs liric API ---- */
    printf("\n=== IN-PROCESS: tcc (libtcc) vs liric API (no process startup) ===\n");
    printf("%-16s %10s %10s %10s | %10s %10s %10s | %7s\n",
           "test", "tcc:comp", "tcc:reloc", "tcc:total",
           "lr:parse", "lr:compile", "lr:total", "ratio");
    printf("%-16s %10s %10s %10s | %10s %10s %10s | %7s\n",
           "----", "--------", "---------", "---------",
           "--------", "----------", "--------", "-----");

    double ip_tcc_total = 0;
    double ip_lr_parse_total = 0, ip_lr_compile_total = 0;

    for (size_t ci = 0; ci < NUM_CASES; ci++) {
        const bench_case_t *tc = &g_cases[ci];
        size_t ll_len = strlen(tc->ll_src);

        double best_tcc_comp = 1e18, best_tcc_reloc = 1e18;
        double best_lr_parse = 1e18, best_lr_compile = 1e18;

        /* TCC in-process via libtcc */
        for (int it = 0; it < iters; it++) {
            TCCState *s = tcc_new();
            tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

            double t0 = now_us();
            tcc_compile_string(s, tc->c_src);
            double t1 = now_us();
            tcc_relocate(s);
            double t2 = now_us();

            double comp = t1 - t0;
            double reloc = t2 - t1;
            if (comp < best_tcc_comp) best_tcc_comp = comp;
            if (reloc < best_tcc_reloc) best_tcc_reloc = reloc;

            tcc_delete(s);
        }

        /* liric in-process via API */
        for (int it = 0; it < iters; it++) {
            char err[256] = {0};

            double t0 = now_us();
            lr_module_t *m = lr_parse_ll(tc->ll_src, ll_len, err, sizeof(err));
            double t1 = now_us();

            if (!m) {
                fprintf(stderr, "  parse error [%s]: %s\n", tc->name, err);
                break;
            }

            lr_jit_t *jit = lr_jit_create();
            double t2 = now_us();
            lr_jit_add_module(jit, m);
            double t3 = now_us();

            double parse_t = t1 - t0;
            double compile_t = t3 - t2;
            if (parse_t < best_lr_parse) best_lr_parse = parse_t;
            if (compile_t < best_lr_compile) best_lr_compile = compile_t;

            lr_jit_destroy(jit);
            lr_module_free(m);
        }

        double tcc_tot = best_tcc_comp + best_tcc_reloc;
        double lr_tot = best_lr_parse + best_lr_compile;
        double r = (lr_tot > 0) ? tcc_tot / lr_tot : 0;
        printf("%-16s %10.1f %10.1f %10.1f | %10.1f %10.1f %10.1f | %6.1fx\n",
               tc->name,
               best_tcc_comp, best_tcc_reloc, tcc_tot,
               best_lr_parse, best_lr_compile, lr_tot, r);

        if (tcc_tot < 1e17) ip_tcc_total += tcc_tot;
        if (best_lr_parse < 1e17) ip_lr_parse_total += best_lr_parse;
        if (best_lr_compile < 1e17) ip_lr_compile_total += best_lr_compile;
    }

    double lr_tot_all = ip_lr_parse_total + ip_lr_compile_total;
    double r_all = (lr_tot_all > 0) ? ip_tcc_total / lr_tot_all : 0;
    printf("%-16s %10s %10s %10s | %10s %10s %10s | %7s\n",
           "----", "--------", "---------", "---------",
           "--------", "----------", "--------", "-----");
    printf("%-16s %10s %10s %10.1f | %10.1f %10.1f %10.1f | %6.1fx\n",
           "TOTAL", "", "", ip_tcc_total,
           ip_lr_parse_total, ip_lr_compile_total, lr_tot_all, r_all);

    printf("\nAll times in microseconds (us). ratio > 1 = liric faster.\n");
    printf("tcc:comp = tcc_compile_string(), tcc:reloc = tcc_relocate()\n");
    printf("lr:parse = lr_parse_ll(), lr:compile = lr_jit_add_module()\n");

    return 0;
}
