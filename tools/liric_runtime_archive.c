#include <liric/liric_session.h>

#include "bc_decode.h"
#include "ir.h"
#include "runtime_archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct file_buf {
    uint8_t *data;
    size_t len;
} file_buf_t;

static int read_file(const char *path, file_buf_t *out) {
    FILE *f = NULL;
    long len = 0;
    uint8_t *buf = NULL;
    size_t got = 0;

    if (!path || !path[0] || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    len = ftell(f);
    if (len <= 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return -1;
    }
    out->data = buf;
    out->len = got;
    return 0;
}

static void free_file(file_buf_t *buf) {
    if (!buf)
        return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
}

static void usage(FILE *out) {
    fprintf(out,
            "usage: liric_runtime_archive --input-bc PATH --output PATH "
            "[--target TARGET] [--backend isel|copy_patch|llvm]\n");
}

static lr_session_backend_t parse_backend(const char *s) {
    if (!s || strcmp(s, "isel") == 0)
        return LR_SESSION_BACKEND_ISEL;
    if (strcmp(s, "copy_patch") == 0)
        return LR_SESSION_BACKEND_COPY_PATCH;
    if (strcmp(s, "llvm") == 0)
        return LR_SESSION_BACKEND_LLVM;
    return -1;
}

int main(int argc, char **argv) {
    const char *input_bc = NULL;
    const char *output = NULL;
    const char *target = NULL;
    const char *backend_s = "isel";
    lr_session_backend_t backend = LR_SESSION_BACKEND_ISEL;
    file_buf_t bc = {0};
    lr_session_config_t cfg;
    lr_error_t err = {0};
    lr_session_t *session = NULL;
    uint8_t *blob_pkg = NULL;
    size_t blob_pkg_len = 0;
    char *ir_buf = NULL;
    size_t ir_len = 0;
    FILE *mem = NULL;
    FILE *out = NULL;
    char parse_err[256] = {0};
    int rc = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input-bc") == 0 && i + 1 < argc) {
            input_bc = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            target = argv[++i];
        } else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_s = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    backend = parse_backend(backend_s);
    if (!input_bc || !output || backend < 0) {
        usage(stderr);
        return 2;
    }

    if (read_file(input_bc, &bc) != 0) {
        fprintf(stderr, "failed to read bitcode: %s\n", input_bc);
        goto done;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = LR_MODE_DIRECT;
    cfg.target = target;
    cfg.backend = backend;
    session = lr_session_create(&cfg, &err);
    if (!session) {
        fprintf(stderr, "session_create failed: %s\n", err.msg);
        goto done;
    }
    if (lr_parse_bc_to_session(bc.data, bc.len, session, parse_err,
                               sizeof(parse_err)) != 0) {
        fprintf(stderr, "runtime bc parse failed: %s\n", parse_err);
        goto done;
    }
    if (lr_session_export_blob_package(session, &blob_pkg, &blob_pkg_len, &err) != 0) {
        fprintf(stderr, "blob export failed: %s\n", err.msg);
        goto done;
    }
    mem = open_memstream(&ir_buf, &ir_len);
    if (!mem) {
        fprintf(stderr, "open_memstream failed\n");
        goto done;
    }
    lr_module_dump(lr_session_module(session), mem);
    if (fclose(mem) != 0) {
        mem = NULL;
        fprintf(stderr, "failed to finalize runtime IR buffer\n");
        goto done;
    }
    mem = NULL;
    if (!ir_buf || ir_len == 0) {
        fprintf(stderr, "runtime IR dump is empty\n");
        goto done;
    }
    out = fopen(output, "wb");
    if (!out) {
        fprintf(stderr, "failed to open output: %s\n", output);
        goto done;
    }
    if (lr_runtime_archive_write(out, ir_buf, ir_len, blob_pkg, blob_pkg_len) != 0) {
        fprintf(stderr, "failed to write runtime archive: %s\n", output);
        goto done;
    }
    if (fclose(out) != 0) {
        out = NULL;
        fprintf(stderr, "failed to finalize runtime archive: %s\n", output);
        goto done;
    }
    out = NULL;
    rc = 0;

done:
    if (mem)
        fclose(mem);
    if (out)
        fclose(out);
    free(ir_buf);
    free(blob_pkg);
    if (session)
        lr_session_destroy(session);
    free_file(&bc);
    return rc;
}
