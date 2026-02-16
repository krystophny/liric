#include "module_emit.h"

#include "compile_mode.h"
#include "llvm_backend.h"
#include "objfile.h"
#include "target.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

static void emit_err(char *err, size_t err_cap, const char *fmt, ...) {
    va_list args;
    if (!err || err_cap == 0)
        return;
    err[0] = '\0';
    va_start(args, fmt);
    (void)vsnprintf(err, err_cap, fmt, args);
    va_end(args);
}

static const lr_target_t *resolve_target(const char *target_name,
                                         char *err,
                                         size_t err_cap) {
    const lr_target_t *target = NULL;
    if (target_name && target_name[0])
        target = lr_target_by_name(target_name);
    else
        target = lr_target_host();
    if (!target)
        emit_err(err, err_cap, "target not found");
    return target;
}

#if defined(__unix__) || defined(__APPLE__)
static int copy_file_to_stream(const char *path, FILE *out) {
    FILE *in = NULL;
    uint8_t buf[8192];
    size_t n = 0;
    if (!path || !out)
        return -1;
    in = fopen(path, "rb");
    if (!in)
        return -1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            (void)fclose(in);
            return -1;
        }
    }
    if (ferror(in) || ferror(out)) {
        (void)fclose(in);
        return -1;
    }
    (void)fclose(in);
    return 0;
}
#endif

int lr_emit_module_object_path_mode(lr_module_t *module,
                                    const char *target_name,
                                    lr_compile_mode_t mode,
                                    const char *path,
                                    char *err,
                                    size_t err_cap) {
    const lr_target_t *target = NULL;
    int rc = -1;
    FILE *out = NULL;

    if (!module || !path) {
        emit_err(err, err_cap, "invalid object emission arguments");
        return -1;
    }
    target = resolve_target(target_name, err, err_cap);
    if (!target)
        return -1;

    if (mode == LR_COMPILE_LLVM) {
        char backend_err[256] = {0};
        rc = lr_llvm_emit_object_path(module, target, path,
                                      backend_err, sizeof(backend_err));
        if (rc != 0) {
            emit_err(err, err_cap, "llvm object emission failed: %s",
                     backend_err[0] ? backend_err : "unknown backend error");
            return -1;
        }
        return 0;
    }

    out = fopen(path, "wb");
    if (!out) {
        emit_err(err, err_cap, "cannot open output file: %s", path);
        return -1;
    }
    rc = lr_emit_object(module, target, out);
    (void)fclose(out);
    if (rc != 0) {
        emit_err(err, err_cap, "object emission failed");
        return -1;
    }
    return 0;
}

int lr_emit_module_object_path(lr_module_t *module,
                               const char *target_name,
                               const char *path,
                               char *err,
                               size_t err_cap) {
    return lr_emit_module_object_path_mode(module, target_name,
                                           lr_compile_mode_from_env(),
                                           path, err, err_cap);
}

int lr_emit_module_object_stream(lr_module_t *module,
                                 const char *target_name,
                                 FILE *out,
                                 char *err,
                                 size_t err_cap) {
    const lr_target_t *target = NULL;
    lr_compile_mode_t mode;

    if (!module || !out) {
        emit_err(err, err_cap, "invalid object stream arguments");
        return -1;
    }
    target = resolve_target(target_name, err, err_cap);
    if (!target)
        return -1;

    mode = lr_compile_mode_from_env();
    if (mode == LR_COMPILE_LLVM) {
#if defined(__unix__) || defined(__APPLE__)
        char tmp_tpl[] = "/tmp/liric_emit_obj_XXXXXX";
        char backend_err[256] = {0};
        int fd = mkstemp(tmp_tpl);
        int rc = -1;
        if (fd < 0) {
            emit_err(err, err_cap, "temporary file creation failed");
            return -1;
        }
        close(fd);
        rc = lr_llvm_emit_object_path(module, target, tmp_tpl,
                                      backend_err, sizeof(backend_err));
        if (rc == 0)
            rc = copy_file_to_stream(tmp_tpl, out);
        unlink(tmp_tpl);
        if (rc != 0) {
            emit_err(err, err_cap, "llvm object stream emission failed: %s",
                     backend_err[0] ? backend_err : "copy failed");
            return -1;
        }
        return 0;
#else
        emit_err(err, err_cap, "llvm object stream emission unsupported");
        return -1;
#endif
    }

    if (lr_emit_object(module, target, out) != 0) {
        emit_err(err, err_cap, "object emission failed");
        return -1;
    }
    return 0;
}

int lr_emit_module_executable_path_mode(lr_module_t *module,
                                        const char *target_name,
                                        lr_compile_mode_t mode,
                                        const char *path,
                                        const char *entry,
                                        const char *runtime_ll,
                                        size_t runtime_len,
                                        char *err,
                                        size_t err_cap) {
    const lr_target_t *target = NULL;
    int rc = -1;
    FILE *out = NULL;
    bool with_runtime = (runtime_ll && runtime_len > 0);

    if (!module || !path || !entry || !entry[0]) {
        emit_err(err, err_cap, "invalid executable emission arguments");
        return -1;
    }
    target = resolve_target(target_name, err, err_cap);
    if (!target)
        return -1;

    if (mode == LR_COMPILE_LLVM) {
        char backend_err[256] = {0};
        rc = lr_llvm_emit_executable_path(module,
                                          with_runtime ? runtime_ll : NULL,
                                          with_runtime ? runtime_len : 0,
                                          target, path, entry,
                                          backend_err, sizeof(backend_err));
        if (rc != 0) {
            emit_err(err, err_cap, "llvm executable emission failed: %s",
                     backend_err[0] ? backend_err : "unknown backend error");
            return -1;
        }
        return 0;
    }

    out = fopen(path, "wb");
    if (!out) {
        emit_err(err, err_cap, "cannot open output file: %s", path);
        return -1;
    }
    if (with_runtime) {
        rc = lr_emit_executable_with_runtime(module, runtime_ll, runtime_len,
                                             target, out, entry);
    } else {
        rc = lr_emit_executable(module, target, out, entry);
    }
    (void)fclose(out);
    if (rc != 0) {
        emit_err(err, err_cap, with_runtime
                 ? "executable emission with runtime failed"
                 : "executable emission failed");
        return -1;
    }
    return 0;
}

int lr_emit_module_executable_path(lr_module_t *module,
                                   const char *target_name,
                                   const char *path,
                                   const char *entry,
                                   const char *runtime_ll,
                                   size_t runtime_len,
                                   char *err,
                                   size_t err_cap) {
    return lr_emit_module_executable_path_mode(module, target_name,
                                               lr_compile_mode_from_env(),
                                               path, entry,
                                               runtime_ll, runtime_len,
                                               err, err_cap);
}
