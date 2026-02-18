#include "bench_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int parse_time_report_total(const char *text, double *out) {
    const char *p = text;
    double total = 0.0;
    int found = 0;
    while ((p = strstr(p, "\"total\":")) != NULL) {
        p += 8;
        while (*p == ' ') p++;
        total = strtod(p, NULL);
        found = 1;
    }
    if (found) *out = total;
    return found;
}

static void usage(void) {
    printf("usage: bench_overhead_probe --lfortran PATH\n");
    printf("  Measures subprocess overhead of a trivial lfortran --jit invocation.\n");
    printf("  Output: JSON lines to stdout (10 iterations + summary).\n");
}

int main(int argc, char **argv) {
    const char *lfortran = NULL;
    int iters = 10;
    int i;
    double *wall_arr;
    double *total_arr;
    double *overhead_arr;
    char src_path[] = "/tmp/liric_probe_XXXXXX.f90";
    int src_fd;
    const char *trivial_src = "program p\nend program\n";

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--lfortran") == 0 && i + 1 < argc) {
            lfortran = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (!lfortran) {
        fprintf(stderr, "error: --lfortran PATH required\n");
        usage();
        return 1;
    }

    src_fd = mkstemps(src_path, 4);
    if (src_fd < 0) {
        perror("mkstemps");
        return 1;
    }
    write(src_fd, trivial_src, strlen(trivial_src));
    close(src_fd);

    wall_arr = (double *)calloc((size_t)iters, sizeof(double));
    total_arr = (double *)calloc((size_t)iters, sizeof(double));
    overhead_arr = (double *)calloc((size_t)iters, sizeof(double));

    for (i = 0; i < iters; i++) {
        bench_cmd_result_t r = {0};
        char *cmd_argv[8];
        bench_run_cmd_opts_t opts = {0};
        double wall_ms, total_ms;

        cmd_argv[0] = (char *)lfortran;
        cmd_argv[1] = (char *)"--time-report";
        cmd_argv[2] = (char *)"--jit";
        cmd_argv[3] = (char *)src_path;
        cmd_argv[4] = NULL;

        opts.argv = cmd_argv;
        opts.timeout_ms = 10000;

        wall_ms = now_ms();
        if (bench_run_cmd(&opts, &r) != 0 || r.rc != 0) {
            fprintf(stderr, "iter %d: lfortran failed (rc=%d)\n", i, r.rc);
            if (r.stderr_text) fprintf(stderr, "  stderr: %s\n", r.stderr_text);
            bench_free_cmd_result(&r);
            continue;
        }
        wall_ms = r.elapsed_ms;

        total_ms = 0.0;
        if (!r.stdout_text || !parse_time_report_total(r.stdout_text, &total_ms)) {
            total_ms = wall_ms;
        }

        wall_arr[i] = wall_ms;
        total_arr[i] = total_ms;
        overhead_arr[i] = wall_ms - total_ms;

        printf("{\"iter\":%d,\"wall_ms\":%.3f,\"time_report_total_ms\":%.3f,\"overhead_ms\":%.3f}\n",
               i, wall_ms, total_ms, wall_ms - total_ms);

        bench_free_cmd_result(&r);
    }

    {
        double med_wall = bench_median(wall_arr, (size_t)iters);
        double med_total = bench_median(total_arr, (size_t)iters);
        double med_overhead = bench_median(overhead_arr, (size_t)iters);

        printf("{\"summary\":true,\"iters\":%d,"
               "\"median_wall_ms\":%.3f,\"median_time_report_total_ms\":%.3f,"
               "\"median_overhead_ms\":%.3f}\n",
               iters, med_wall, med_total, med_overhead);
    }

    free(wall_arr);
    free(total_arr);
    free(overhead_arr);
    unlink(src_path);

    return 0;
}
