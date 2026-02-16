#ifndef LIRIC_BENCH_COMMON_H
#define LIRIC_BENCH_COMMON_H

#include <stddef.h>

typedef struct {
    int rc;
    char *stdout_text;
    char *stderr_text;
    double elapsed_ms;
    int timed_out;
} bench_cmd_result_t;

typedef struct {
    char *const *argv;
    int timeout_ms;
    int timeout_grace_ms;
    const char *stdout_path;
    const char *env_lib_dir;
    const char *work_dir;
} bench_run_cmd_opts_t;

char *bench_xstrdup(const char *s);
char *bench_to_abs_path(const char *path);
char *bench_path_join2(const char *a, const char *b);
char *bench_dirname_dup(const char *path);
char *bench_read_all_file(const char *path);

int bench_run_cmd(const bench_run_cmd_opts_t *opts, bench_cmd_result_t *out);
void bench_free_cmd_result(bench_cmd_result_t *r);

double bench_median(const double *vals, size_t n);
double bench_percentile(const double *vals, size_t n, double p);
int bench_json_get_number(const char *json, const char *key, double *out_val);

#endif
