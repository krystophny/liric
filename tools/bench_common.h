#ifndef LIRIC_BENCH_COMMON_H
#define LIRIC_BENCH_COMMON_H

#include <stddef.h>

#define BENCH_MODE_COUNT 3

typedef enum {
    BENCH_MODE_ISEL = 0,
    BENCH_MODE_COPY_PATCH = 1,
    BENCH_MODE_LLVM = 2
} bench_mode_id_t;

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
int bench_run_cmd_with_mode(const char *mode, const bench_run_cmd_opts_t *opts, bench_cmd_result_t *out);
void bench_free_cmd_result(bench_cmd_result_t *r);

int bench_is_supported_mode(const char *mode);
const char *bench_mode_name(size_t mode_idx);
int bench_parse_modes_csv(const char *csv, int *modes_out, size_t modes_len);

double bench_median(const double *vals, size_t n);
double bench_percentile(const double *vals, size_t n, double p);
int bench_json_get_number(const char *json, const char *key, double *out_val);

#endif
