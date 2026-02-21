#include "bc_decode.h"
#include "ll_parser.h"
#include "wasm_decode.h"
#include "wasm_to_ir.h"
#include <liric/liric.h>
#include <liric/liric_session.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lr_compiler {
    lr_session_t *session;
    lr_policy_t policy;
    lr_backend_t backend;
};

static void compiler_err_clear(lr_compiler_error_t *err) {
    if (!err)
        return;
    err->code = LR_COMPILER_OK;
    err->msg[0] = '\0';
}

static void compiler_err_set(lr_compiler_error_t *err, int code,
                             const char *msg) {
    if (!err)
        return;
    err->code = code;
    if (!msg) {
        err->msg[0] = '\0';
        return;
    }
    (void)snprintf(err->msg, sizeof(err->msg), "%s", msg);
}

static void compiler_err_from_session(lr_compiler_error_t *err,
                                      const lr_error_t *serr) {
    if (!err)
        return;
    if (!serr) {
        compiler_err_set(err, LR_COMPILER_ERR_BACKEND, "unknown session error");
        return;
    }
    switch (serr->code) {
    case LR_OK:
        compiler_err_set(err, LR_COMPILER_OK, "");
        break;
    case LR_ERR_ARGUMENT:
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, serr->msg);
        break;
    case LR_ERR_STATE:
        compiler_err_set(err, LR_COMPILER_ERR_STATE, serr->msg);
        break;
    case LR_ERR_MODE:
        compiler_err_set(err, LR_COMPILER_ERR_UNSUPPORTED, serr->msg);
        break;
    case LR_ERR_NOT_FOUND:
        compiler_err_set(err, LR_COMPILER_ERR_NOT_FOUND, serr->msg);
        break;
    case LR_ERR_PARSE:
        compiler_err_set(err, LR_COMPILER_ERR_PARSE, serr->msg);
        break;
    case LR_ERR_BACKEND:
    default:
        compiler_err_set(err, LR_COMPILER_ERR_BACKEND, serr->msg);
        break;
    }
}

static int backend_to_session_backend(lr_backend_t backend,
                                      lr_session_backend_t *out_backend) {
    if (!out_backend)
        return -1;
    switch (backend) {
    case LR_BACKEND_ISEL:
        *out_backend = LR_SESSION_BACKEND_ISEL;
        return 0;
    case LR_BACKEND_COPY_PATCH:
        *out_backend = LR_SESSION_BACKEND_COPY_PATCH;
        return 0;
    case LR_BACKEND_LLVM:
        *out_backend = LR_SESSION_BACKEND_LLVM;
        return 0;
    default:
        return -1;
    }
}

lr_compiler_t *lr_compiler_create(const lr_compiler_config_t *cfg,
                                  lr_compiler_error_t *err) {
    lr_compiler_t *c = NULL;
    lr_session_config_t scfg;
    lr_error_t serr = {0};
    lr_policy_t policy = LR_POLICY_DIRECT;
    lr_backend_t backend = LR_BACKEND_ISEL;
    const char *target = NULL;

    compiler_err_clear(err);

    if (cfg) {
        policy = cfg->policy;
        backend = cfg->backend;
        target = cfg->target;
    }

    if (policy != LR_POLICY_DIRECT && policy != LR_POLICY_IR) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid policy");
        return NULL;
    }
    memset(&scfg, 0, sizeof(scfg));
    scfg.mode = (policy == LR_POLICY_DIRECT) ? LR_MODE_DIRECT : LR_MODE_IR;
    scfg.target = target;
    if (backend_to_session_backend(backend, &scfg.backend) != 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid backend");
        return NULL;
    }

    c = (lr_compiler_t *)calloc(1, sizeof(*c));
    if (!c) {
        compiler_err_set(err, LR_COMPILER_ERR_BACKEND, "compiler allocation failed");
        return NULL;
    }

    c->session = lr_session_create(&scfg, &serr);
    if (!c->session) {
        compiler_err_from_session(err, &serr);
        free(c);
        return NULL;
    }

    c->policy = policy;
    c->backend = backend;
    return c;
}

void lr_compiler_destroy(lr_compiler_t *c) {
    if (!c)
        return;
    if (c->session)
        lr_session_destroy(c->session);
    free(c);
}

int lr_compiler_add_symbol(lr_compiler_t *c, const char *name, void *addr) {
    if (!c || !c->session || !name || !name[0])
        return -1;
    lr_session_add_symbol(c->session, name, addr);
    return 0;
}

int lr_compiler_load_library(lr_compiler_t *c, const char *path,
                             lr_compiler_error_t *err) {
    lr_jit_t *jit = NULL;
    compiler_err_clear(err);
    if (!c || !c->session || !path || !path[0]) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT,
                         "invalid load_library arguments");
        return -1;
    }
    jit = lr_session_jit(c->session);
    if (!jit) {
        compiler_err_set(err, LR_COMPILER_ERR_STATE, "compiler has no JIT");
        return -1;
    }
    if (lr_jit_load_library(jit, path) != 0) {
        compiler_err_set(err, LR_COMPILER_ERR_BACKEND, "failed to load library");
        return -1;
    }
    return 0;
}

int lr_compiler_set_runtime_bc(lr_compiler_t *c, const uint8_t *bc_data,
                               size_t bc_len, lr_compiler_error_t *err) {
    lr_error_t sess_err = {0};
    compiler_err_clear(err);
    if (!c || !c->session || !bc_data || bc_len == 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT,
                         "invalid runtime bc arguments");
        return -1;
    }
    if (lr_session_set_runtime_bc(c->session, bc_data, bc_len, &sess_err) != 0) {
        compiler_err_from_session(err, &sess_err);
        return -1;
    }
    return 0;
}

int lr_compiler_feed_ll(lr_compiler_t *c, const char *src, size_t len,
                        lr_compiler_error_t *err) {
    char parse_err[512] = {0};
    compiler_err_clear(err);
    if (!c || !c->session || !src || len == 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid ll input");
        return -1;
    }
    if (lr_parse_ll_to_session(src, len, c->session, parse_err, sizeof(parse_err)) != 0) {
        compiler_err_set(err, LR_COMPILER_ERR_PARSE,
                         parse_err[0] ? parse_err : "ll streaming parse failed");
        return -1;
    }
    return 0;
}

int lr_compiler_feed_bc(lr_compiler_t *c, const uint8_t *data, size_t len,
                        lr_compiler_error_t *err) {
    char parse_err[512] = {0};
    compiler_err_clear(err);
    if (!c || !c->session || !data || len == 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid bc input");
        return -1;
    }
    if (lr_parse_bc_to_session(data, len, c->session, parse_err, sizeof(parse_err)) != 0) {
        compiler_err_set(err, LR_COMPILER_ERR_PARSE,
                         parse_err[0] ? parse_err : "bc streaming parse failed");
        return -1;
    }
    return 0;
}

int lr_compiler_feed_wasm(lr_compiler_t *c, const uint8_t *data, size_t len,
                          lr_compiler_error_t *err) {
    lr_arena_t *wasm_arena = NULL;
    lr_wasm_module_t *wmod = NULL;
    char parse_err[512] = {0};
    lr_error_t sess_err = {0};

    compiler_err_clear(err);
    if (!c || !c->session || !data || len == 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid wasm input");
        return -1;
    }

    wasm_arena = lr_arena_create(0);
    if (!wasm_arena) {
        compiler_err_set(err, LR_COMPILER_ERR_BACKEND, "wasm arena allocation failed");
        return -1;
    }

    wmod = lr_wasm_decode(data, len, wasm_arena, parse_err, sizeof(parse_err));
    if (!wmod) {
        lr_arena_destroy(wasm_arena);
        compiler_err_set(err, LR_COMPILER_ERR_PARSE,
                         parse_err[0] ? parse_err : "wasm decode failed");
        return -1;
    }

    if (lr_wasm_to_session(wmod, c->session, NULL, &sess_err) != 0) {
        lr_arena_destroy(wasm_arena);
        compiler_err_from_session(err, &sess_err);
        return -1;
    }

    lr_arena_destroy(wasm_arena);
    return 0;
}

static int is_wasm_binary(const uint8_t *data, size_t len) {
    return data && len >= 4 &&
           data[0] == 0x00 &&
           data[1] == 'a' &&
           data[2] == 's' &&
           data[3] == 'm';
}

int lr_compiler_feed_auto(lr_compiler_t *c, const uint8_t *data, size_t len,
                          lr_compiler_error_t *err) {
    lr_error_t sess_err = {0};
    if (!c || !data || len == 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid auto input");
        return -1;
    }
    if (c->policy == LR_POLICY_IR) {
        if (lr_session_compile_auto(c->session, data, len, NULL, &sess_err) != 0) {
            compiler_err_from_session(err, &sess_err);
            return -1;
        }
        compiler_err_clear(err);
        return 0;
    }
    if (is_wasm_binary(data, len))
        return lr_compiler_feed_wasm(c, data, len, err);
    if (lr_bc_is_bitcode(data, len))
        return lr_compiler_feed_bc(c, data, len, err);
    return lr_compiler_feed_ll(c, (const char *)data, len, err);
}

void *lr_compiler_lookup(lr_compiler_t *c, const char *name) {
    if (!c || !c->session || !name || !name[0])
        return NULL;
    return lr_session_lookup(c->session, name);
}

int lr_compiler_emit_object(lr_compiler_t *c, const char *path,
                            lr_compiler_error_t *err) {
    lr_error_t sess_err = {0};
    compiler_err_clear(err);
    if (!c || !c->session || !path || !path[0]) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid emit_object arguments");
        return -1;
    }
    if (lr_session_emit_object(c->session, path, &sess_err) != 0) {
        compiler_err_from_session(err, &sess_err);
        return -1;
    }
    return 0;
}

int lr_compiler_emit_exe(lr_compiler_t *c, const char *path,
                         lr_compiler_error_t *err) {
    lr_error_t sess_err = {0};
    compiler_err_clear(err);
    if (!c || !c->session || !path || !path[0]) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT, "invalid emit_exe arguments");
        return -1;
    }
    if (lr_session_emit_exe(c->session, path, &sess_err) != 0) {
        compiler_err_from_session(err, &sess_err);
        return -1;
    }
    return 0;
}

int lr_compiler_emit_exe_with_runtime(lr_compiler_t *c, const char *path,
                                      const char *runtime_ll, size_t runtime_len,
                                      lr_compiler_error_t *err) {
    lr_error_t sess_err = {0};
    compiler_err_clear(err);
    if (!c || !c->session || !path || !path[0] || !runtime_ll || runtime_len == 0) {
        compiler_err_set(err, LR_COMPILER_ERR_ARGUMENT,
                         "invalid emit_exe_with_runtime arguments");
        return -1;
    }
    if (lr_session_emit_exe_with_runtime(c->session, path, runtime_ll, runtime_len,
                                         &sess_err) != 0) {
        compiler_err_from_session(err, &sess_err);
        return -1;
    }
    return 0;
}

lr_policy_t lr_compiler_policy(const lr_compiler_t *c) {
    return c ? c->policy : LR_POLICY_DIRECT;
}

lr_backend_t lr_compiler_backend(const lr_compiler_t *c) {
    return c ? c->backend : LR_BACKEND_ISEL;
}
