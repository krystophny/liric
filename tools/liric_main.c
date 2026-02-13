#include "../src/arena.h"
#include "../src/bc_decode.h"
#include "../src/ir.h"
#include "../src/jit.h"
#include "../src/liric.h"
#include "../src/ll_parser.h"
#include "../src/objfile.h"
#include "../src/target.h"
#include <liric/liric_session.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    *out_len = n;
    return buf;
}

static char *read_stdin(size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    while (!feof(stdin)) {
        if (len + 1024 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) return NULL;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, stdin);
        len += n;
        if (n == 0) break;
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

static bool module_has_main_definition(const lr_module_t *m) {
    for (const lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, "main") == 0 && !f->is_decl &&
            f->first_block) {
            return true;
        }
    }
    return false;
}

static bool output_path_forces_object(const char *path) {
    size_t len = 0;
    if (!path)
        return false;
    len = strlen(path);
    return len >= 2 && path[len - 2] == '.' && path[len - 1] == 'o';
}

static bool is_wasm_binary(const uint8_t *data, size_t len) {
    return data && len >= 4 &&
           data[0] == 0x00 &&
           data[1] == 'a' &&
           data[2] == 's' &&
           data[3] == 'm';
}

static void dump_module_functions(lr_module_t *m, FILE *out) {
    if (!m || !out)
        return;
    for (lr_func_t *f = m->first_func; f; f = f->next)
        lr_dump_func(f, m, out);
}

typedef struct ll_dump_ctx {
    FILE *out;
} ll_dump_ctx_t;

static int ll_dump_callback(lr_func_t *func, lr_module_t *mod, void *ctx_ptr) {
    ll_dump_ctx_t *ctx = (ll_dump_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->out || !func || !mod)
        return -1;
    lr_dump_func(func, mod, ctx->out);
    return 0;
}

static int dump_ir_ll_streaming(const char *src, size_t len,
                                char *err, size_t err_cap) {
    ll_dump_ctx_t ctx;
    lr_module_t *m;
    if (!src || len == 0)
        return -1;
    ctx.out = stdout;
    m = lr_parse_ll_streaming(src, len, ll_dump_callback, &ctx, err, err_cap);
    if (!m)
        return -1;
    lr_module_free(m);
    return 0;
}

typedef struct bc_dump_ctx {
    FILE *out;
    const lr_func_t *cur_func;
    const lr_block_t *cur_block;
} bc_dump_ctx_t;

static int bc_dump_callback(lr_func_t *func, lr_block_t *block,
                            const lr_bc_inst_desc_t *inst, void *ctx_ptr) {
    bc_dump_ctx_t *ctx = (bc_dump_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->out || !func || !block || !inst)
        return -1;

    if (ctx->cur_func != func) {
        if (ctx->cur_func)
            fprintf(ctx->out, "}\n");
        lr_dump_func_signature(func, ctx->out);
        fprintf(ctx->out, " {\n");
        ctx->cur_func = func;
        ctx->cur_block = NULL;
    }
    if (ctx->cur_block != block) {
        lr_dump_block_label(block, ctx->out);
        ctx->cur_block = block;
    }

    if (inst->op == LR_OP_RET_VOID) {
        fprintf(ctx->out, "  ret void\n");
    } else if (inst->op == LR_OP_RET &&
               inst->num_operands == 1 &&
               inst->operands &&
               inst->operands[0].kind == LR_OP_KIND_IMM_I64 &&
               inst->operands[0].type &&
               inst->operands[0].type->kind == LR_TYPE_I32) {
        fprintf(ctx->out, "  ret i32 %lld\n",
                (long long)inst->operands[0].imm_i64);
    } else {
        fprintf(ctx->out, "  ; op %u\n", (unsigned)inst->op);
    }
    return 0;
}

static int dump_ir_bc_streaming(const uint8_t *data, size_t len,
                                char *err, size_t err_cap) {
    bc_dump_ctx_t ctx;
    lr_arena_t *arena;
    lr_module_t *m;

    arena = lr_arena_create(0);
    if (!arena) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "arena allocation failed");
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.out = stdout;
    m = lr_parse_bc_streaming(data, len, arena, bc_dump_callback, &ctx,
                              err, err_cap);
    if (!m) {
        lr_arena_destroy(arena);
        return -1;
    }

    if (ctx.cur_func)
        fprintf(ctx.out, "}\n");
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->is_decl || !f->first_block)
            lr_dump_func(f, m, ctx.out);
    }
    lr_module_free(m);
    return 0;
}

int main(int argc, char **argv) {
    bool jit_mode = false;
    bool dump_ir = false;
    const char *output_path_opt = NULL;
    const char *target_name = NULL;
    const char *input_file = NULL;
    const char *func_name = "main";
    const char *runtime_path = NULL;
    const char *load_libs[64];
    int num_load_libs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--jit") == 0) jit_mode = true;
        else if (strcmp(argv[i], "--dump-ir") == 0) dump_ir = true;
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target_name = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_path_opt = argv[++i];
        else if (strcmp(argv[i], "--func") == 0 && i + 1 < argc) func_name = argv[++i];
        else if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) runtime_path = argv[++i];
        else if (strcmp(argv[i], "--load-lib") == 0 && i + 1 < argc) {
            if (num_load_libs < 64)
                load_libs[num_load_libs++] = argv[++i];
            else
                return 1;
        }
        else if (strcmp(argv[i], "-") == 0) input_file = NULL;
        else if (argv[i][0] != '-') input_file = argv[i];
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (output_path_opt && (jit_mode || dump_ir)) {
        fprintf(stderr, "-o is only valid for file output mode\n");
        return 1;
    }

    size_t src_len;
    char *src;
    if (input_file)
        src = read_file(input_file, &src_len);
    else
        src = read_stdin(&src_len);
    if (!src) {
        fprintf(stderr, "failed to read input\n");
        return 1;
    }

    char err[512] = {0};
    if (dump_ir && !runtime_path) {
        int dump_rc = -1;
        if (lr_bc_is_bitcode((const uint8_t *)src, src_len)) {
            dump_rc = dump_ir_bc_streaming((const uint8_t *)src, src_len,
                                           err, sizeof(err));
        } else if (!is_wasm_binary((const uint8_t *)src, src_len)) {
            dump_rc = dump_ir_ll_streaming(src, src_len, err, sizeof(err));
        }
        if (dump_rc == 0) {
            free(src);
            return 0;
        }
        if (err[0] != '\0') {
            fprintf(stderr, "parse error: %s\n", err);
            free(src);
            return 1;
        }
    }

    bool is_ll_text = !is_wasm_binary((const uint8_t *)src, src_len) &&
                       !lr_bc_is_bitcode((const uint8_t *)src, src_len);

    if (jit_mode && is_ll_text && !runtime_path) {
        lr_session_config_t cfg = {0};
        cfg.mode = LR_MODE_DIRECT;
        cfg.target = target_name;
        lr_error_t serr;
        lr_session_t *sess = lr_session_create(&cfg, &serr);
        if (!sess) {
            fprintf(stderr, "session creation failed: %s\n", serr.msg);
            free(src);
            return 1;
        }

        for (int i = 0; i < num_load_libs; i++) {
            lr_session_add_symbol(sess, load_libs[i], NULL);
        }

        int parse_rc = lr_parse_ll_to_session(src, src_len, sess,
                                               err, sizeof(err));
        if (parse_rc != 0) {
            fprintf(stderr, "streaming parse error: %s\n", err);
            lr_session_destroy(sess);
            free(src);
            return 1;
        }

        void *fn_addr = lr_session_lookup(sess, func_name);
        if (!fn_addr) {
            fprintf(stderr, "function '%s' not found\n", func_name);
            lr_session_destroy(sess);
            free(src);
            return 1;
        }

        typedef int (*fn_t)(void);
        fn_t fn;
        memcpy(&fn, &fn_addr, sizeof(fn_addr));
        int result = fn();
        printf("%d\n", result);

        lr_session_destroy(sess);
        free(src);
        return 0;
    }

    lr_module_t *m = lr_parse_auto((const uint8_t *)src, src_len, err, sizeof(err));
    if (!m) {
        fprintf(stderr, "parse error: %s\n", err);
        free(src);
        return 1;
    }

    if (runtime_path) {
        size_t rt_len;
        char *rt_src = read_file(runtime_path, &rt_len);
        if (!rt_src) {
            fprintf(stderr, "failed to read runtime: %s\n", runtime_path);
            lr_module_free(m);
            free(src);
            return 1;
        }
        char rt_err[512] = {0};
        lr_module_t *rt = lr_parse_ll(rt_src, rt_len, rt_err, sizeof(rt_err));
        free(rt_src);
        if (!rt) {
            fprintf(stderr, "runtime parse error: %s\n", rt_err);
            lr_module_free(m);
            free(src);
            return 1;
        }
        if (lr_module_merge(m, rt) != 0) {
            fprintf(stderr, "runtime merge failed\n");
            lr_module_free(rt);
            lr_module_free(m);
            free(src);
            return 1;
        }
        lr_module_free(rt);
    }

    if (dump_ir) {
        dump_module_functions(m, stdout);
        lr_module_free(m);
        free(src);
        return 0;
    }

    if (jit_mode) {
        lr_jit_t *jit = target_name ? lr_jit_create_for_target(target_name) : lr_jit_create();
        if (!jit) {
            fprintf(stderr, "failed to create JIT for target %s\n", target_name ? target_name : "<host>");
            lr_module_free(m);
            free(src);
            return 1;
        }

        for (int i = 0; i < num_load_libs; i++) {
            if (lr_jit_load_library(jit, load_libs[i]) != 0) {
                fprintf(stderr, "failed to load library: %s\n", load_libs[i]);
                lr_jit_destroy(jit);
                lr_module_free(m);
                free(src);
                return 1;
            }
        }

        int rc = lr_jit_add_module(jit, m);
        if (rc != 0) {
            fprintf(stderr, "JIT compilation failed\n");
            lr_jit_destroy(jit);
            lr_module_free(m);
            free(src);
            return 1;
        }

        typedef int (*fn_t)(void);
        fn_t fn; LR_JIT_GET_FN(fn, jit, func_name);
        if (!fn) {
            fprintf(stderr, "function '%s' not found\n", func_name);
            lr_jit_destroy(jit);
            lr_module_free(m);
            free(src);
            return 1;
        }

        int result = fn();
        printf("%d\n", result);

        lr_jit_destroy(jit);
        lr_module_free(m);
        free(src);
        return 0;
    }

    const lr_target_t *target = target_name ? lr_target_by_name(target_name) : lr_target_host();
    if (!target) {
        fprintf(stderr, "unknown target: %s\n", target_name ? target_name : "<host>");
        lr_module_free(m);
        free(src);
        return 1;
    }

    const char *out_path = output_path_opt ? output_path_opt : "a.out";
    bool emit_object = output_path_forces_object(out_path) ||
                       !module_has_main_definition(m);
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "failed to open output: %s\n", out_path);
        lr_module_free(m);
        free(src);
        return 1;
    }

    int emit_rc = emit_object ? lr_emit_object(m, target, out)
                              : lr_emit_executable(m, target, out, func_name);
    fclose(out);
    if (emit_rc != 0) {
        fprintf(stderr, emit_object ? "object emission failed\n"
                                    : "executable emission failed\n");
        lr_module_free(m);
        free(src);
        return 1;
    }

#if !defined(_WIN32)
    if (!emit_object) {
        if (chmod(out_path, 0755) != 0) {
            fprintf(stderr, "failed to chmod executable output: %s\n", out_path);
            lr_module_free(m);
            free(src);
            return 1;
        }
    }
#endif

    lr_module_free(m);
    free(src);
    return 0;
}
