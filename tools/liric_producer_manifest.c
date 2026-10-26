#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "bench_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Producer readiness is a build result, never a semantic parity result. */
enum { INCLUDE, ARCHIVE, COMPILER, TARGET, RUNTIME, RUNTIME_TOOL,
       PRODUCER_LLVM, PRODUCER_LIRIC, FIELD_COUNT };
static const char *keys[] = {"include", "archive", "compiler", "target",
    "runtime", "runtime-tool", "producer-llvm", "producer-liric"};
typedef struct {
    char *field[FIELD_COUNT];
    char *link[64];
    size_t links;
} manifest_t;

static int fail(const char *reason, const char *detail) {
    fprintf(stderr, "PRODUCER_BUILD_FAIL: %s%s%s\n", reason,
            detail ? ": " : "", detail ? detail : "");
    return 1;
}

static int run_producer(char **argv) {
    pid_t pid = fork();
    int status;
    if (pid < 0) return fail("cannot start producer command", argv[0]);
    if (!pid) {
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return fail("cannot wait for producer command", argv[0]);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        return fail("producer command failed", argv[0]);
    return 0;
}

static int set_value(manifest_t *m, const char *key, const char *value) {
    if (!value[0] || strchr(value, '\n') || strchr(value, '\r'))
        return fail("invalid manifest value", key);
    if (strcmp(key, "link-arg") == 0) {
        if (m->links == 64) return fail("too many linker arguments", NULL);
        m->link[m->links++] = bench_xstrdup(value);
        return 0;
    }
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if (strcmp(key, keys[i]) != 0) continue;
        if (m->field[i]) return fail("duplicate manifest field", key);
        m->field[i] = bench_xstrdup(value);
        return 0;
    }
    return fail("unknown manifest field", key);
}

static int read_manifest(manifest_t *m, const char *path) {
    char line[8192];
    FILE *f = fopen(path, "r");
    if (!f) return fail("manifest is unavailable", path);
    if (!fgets(line, sizeof(line), f) || strcmp(line, "liric-producer-v1\n")) {
        fclose(f);
        return fail("unsupported producer manifest", path);
    }
    while (fgets(line, sizeof(line), f)) {
        char *equal = strchr(line, '=');
        size_t length = strlen(line);
        if (!equal || !length || line[length - 1] != '\n') {
            fclose(f);
            return fail("malformed producer manifest", path);
        }
        line[length - 1] = '\0';
        *equal = '\0';
        if (set_value(m, line, equal + 1)) {
            fclose(f);
            return 1;
        }
    }
    int rc = ferror(f);
    fclose(f);
    return rc ? fail("cannot read producer manifest", path) : 0;
}

static int require_file(const char *path, int executable) {
    struct stat st;
    if (path[0] != '/' || stat(path, &st) || !S_ISREG(st.st_mode) || !st.st_size)
        return fail("required artifact is missing or not an absolute file", path);
    if (access(path, executable ? X_OK : R_OK))
        return fail("required artifact is inaccessible", path);
    return 0;
}

static int command(char **argv, const char *dir, const char *expected,
                   int substring) {
    bench_run_cmd_opts_t options = {.argv = argv, .timeout_ms = 30000,
                                   .work_dir = dir};
    bench_cmd_result_t result = {0};
    int rc = bench_run_cmd(&options, &result);
    if (rc || result.rc || result.timed_out ||
        (expected && (!result.stdout_text ||
            (substring ? !strstr(result.stdout_text, expected)
                       : strcmp(result.stdout_text, expected))))) {
        fprintf(stderr, "PRODUCER_BUILD_FAIL: command %s (rc=%d)\n%s%s",
                argv[0], result.rc,
                result.stderr_text ? result.stderr_text : "",
                result.stdout_text ? result.stdout_text : "");
        bench_free_cmd_result(&result);
        return 1;
    }
    bench_free_cmd_result(&result);
    return 0;
}

static const char smoke_source[] =
    "#include <liric/liric_session.h>\n"
    "int main(void) {\n"
    "lr_error_t e={0}; lr_session_config_t c={0};\n"
    "lr_session_t *s=lr_session_create(&c,&e); if(!s)return 1;\n"
    "lr_type_t *i=lr_type_i32_s(s); void *p=0; int (*f)(void);\n"
    "if(lr_session_func_begin(s,\"answer\",i,0,0,0,&e))return 2;\n"
    "if(lr_session_set_block(s,lr_session_block(s),&e))return 3;\n"
    "lr_emit_ret(s,LR_IMM(42,i));\n"
    "if(lr_session_func_end(s,&p,&e)||!p)return 4;\n"
    "memcpy(&f,&p,sizeof(f)); int answer=f();\n"
    "lr_session_destroy(s); printf(\"%d\\n\",answer); return answer!=42; }\n";

static int verify(manifest_t *m, const char *manifest_path) {
    for (size_t i = 0; i < FIELD_COUNT; i++)
        if (!m->field[i]) return fail("required manifest field is absent", keys[i]);
    for (size_t i = ARCHIVE; i < FIELD_COUNT; i++) {
        if (i == TARGET) continue;
        if (require_file(m->field[i], i == COMPILER || i == RUNTIME_TOOL ||
                         i == PRODUCER_LLVM || i == PRODUCER_LIRIC)) return 1;
    }
    char *header = bench_path_join2(m->field[INCLUDE], "liric/liric_session.h");
    int rc = require_file(header, 0);
    free(header);
    if (rc) return rc;
    for (size_t i = PRODUCER_LLVM; i <= PRODUCER_LIRIC; i++) {
        char *version[] = {m->field[i], "--version", NULL};
        if (command(version, NULL, "LFortran", 1)) return 1;
    }
    char *directory = bench_dirname_dup(manifest_path);
    char *scratch = bench_path_join2(directory, ".producer-smoke-XXXXXX");
    free(directory);
    if (!mkdtemp(scratch)) {
        free(scratch);
        return fail("cannot create private producer smoke directory", manifest_path);
    }
    char *source = bench_path_join2(scratch, "session.c");
    char *binary = bench_path_join2(scratch, "session");
    char *include = malloc(strlen(m->field[INCLUDE]) + 3);
    if (!include) exit(1);
    sprintf(include, "-I%s", m->field[INCLUDE]);
    FILE *f = fopen(source, "w");
    if (!f) {
        rc = fail("cannot write producer smoke", source);
    } else {
        if (fputs(smoke_source, f) == EOF) rc = 1;
        if (fclose(f)) rc = 1;
        char *argv[76] = {m->field[COMPILER], "-std=c11", include, source,
                          m->field[ARCHIVE]};
        size_t argc = 5;
        for (size_t i = 0; i < m->links; i++) argv[argc++] = m->link[i];
        argv[argc++] = "-o";
        argv[argc++] = binary;
        argv[argc] = NULL;
        if (!rc) rc = command(argv, scratch, NULL, 0);
        char *run[] = {binary, NULL};
        if (!rc) rc = command(run, scratch, "42\n", 0);
    }
    unlink(source);
    unlink(binary);
    rmdir(scratch);
    free(source);
    free(binary);
    free(include);
    free(scratch);
    return rc;
}

static int publish(manifest_t *m, const char *path) {
    char *temporary = malloc(strlen(path) + 32);
    if (!temporary) exit(1);
    sprintf(temporary, "%s.%ld.new", path, (long)getpid());
    FILE *f = fopen(temporary, "w");
    if (!f) {
        free(temporary);
        return fail("cannot publish producer manifest", path);
    }
    int rc = fputs("liric-producer-v1\n", f) == EOF;
    for (size_t i = 0; i < FIELD_COUNT; i++)
        if (fprintf(f, "%s=%s\n", keys[i], m->field[i]) < 0) rc = 1;
    for (size_t i = 0; i < m->links; i++)
        if (fprintf(f, "link-arg=%s\n", m->link[i]) < 0) rc = 1;
    if (fclose(f)) rc = 1;
    if (!rc && rename(temporary, path)) rc = 1;
    if (rc) unlink(temporary);
    free(temporary);
    return rc ? fail("cannot publish producer manifest", path) : 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--run"))
        return run_producer(argv + 2);
    manifest_t m = {0};
    const char *path = NULL;
    int check = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        if (i + 1 == argc || strncmp(argv[i], "--", 2))
            return fail("expected --field value", argv[i]);
        const char *key = argv[i] + 2;
        const char *value = argv[++i];
        if (!strcmp(key, "publish") || !strcmp(key, "check")) {
            if (path) return fail("provide one manifest operation", NULL);
            path = value;
            check = !strcmp(key, "check");
            if (!check) unlink(path);
        } else if (set_value(&m, key, value)) return 1;
    }
    if (!path) return fail("--publish or --check is required", NULL);
    if (check) {
        for (size_t i = 0; i < FIELD_COUNT; i++)
            if (m.field[i]) return fail("check consumes manifest fields only", NULL);
        if (m.links) return fail("check consumes manifest linker arguments only", NULL);
        rc = read_manifest(&m, path);
    }
    if (!rc) rc = verify(&m, path);
    if (!rc && !check) rc = publish(&m, path);
    if (!rc) printf("PRODUCER_READY: %s (%s)\n", path, m.field[TARGET]);
    for (size_t i = 0; i < FIELD_COUNT; i++) free(m.field[i]);
    for (size_t i = 0; i < m.links; i++) free(m.link[i]);
    return rc;
}
