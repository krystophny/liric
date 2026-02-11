#include "arena.h"
#include "ir.h"
#include "jit.h"
#include "liric.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Replicate public operand descriptor types to avoid ir.h/liric.h enum clashes. */
typedef struct lr_operand_desc {
    int kind;
    union {
        uint32_t vreg;
        int64_t imm_i64;
        double imm_f64;
        uint32_t block_id;
        uint32_t global_id;
    };
    lr_type_t *type;
} lr_operand_desc_t;

enum {
    LR_OP_KIND_VREG = 0,
    LR_OP_KIND_IMM_I64 = 1,
    LR_OP_KIND_IMM_F64 = 2,
    LR_OP_KIND_BLOCK = 3,
    LR_OP_KIND_GLOBAL = 4,
    LR_OP_KIND_NULL = 5,
    LR_OP_KIND_UNDEF = 6,
};

typedef struct lr_compile_session lr_compile_session_t;

typedef enum lr_compile_strategy {
    LR_COMPILE_STRATEGY_DIRECT_PASS = 0,
    LR_COMPILE_STRATEGY_IR_MODE = 1,
} lr_compile_strategy_t;

typedef enum lr_compile_error_code {
    LR_COMPILE_OK = 0,
    LR_COMPILE_ERR_INVALID_ARGUMENT,
    LR_COMPILE_ERR_MODE_CONFLICT,
    LR_COMPILE_ERR_STATE,
    LR_COMPILE_ERR_NOT_FOUND,
    LR_COMPILE_ERR_BACKEND,
    LR_COMPILE_ERR_PARSE,
    LR_COMPILE_ERR_UNSUPPORTED,
} lr_compile_error_code_t;

typedef struct lr_compile_error {
    lr_compile_error_code_t code;
    char message[256];
} lr_compile_error_t;

typedef struct lr_compile_config {
    lr_compile_strategy_t strategy;
    const char *target_name;
    bool enable_local_peephole;
    bool enable_ir_pipeline;
} lr_compile_config_t;

typedef struct lr_function_spec {
    const char *name;
    lr_type_t *ret_type;
    lr_type_t **param_types;
    uint32_t num_params;
    bool vararg;
} lr_function_spec_t;

typedef struct lr_symbol_handle {
    const char *name;
    void *addr;
} lr_symbol_handle_t;

typedef uint32_t lr_block_id_t;

typedef struct lr_inst_desc {
    lr_opcode_t op;
    lr_type_t *type;
    uint32_t dest;
    const lr_operand_desc_t *operands;
    uint32_t num_operands;
    const uint32_t *indices;
    uint32_t num_indices;
    int icmp_pred;
    int fcmp_pred;
    bool call_external_abi;
    bool call_vararg;
} lr_inst_desc_t;

typedef struct lr_ir_pipeline {
    uint32_t opt_level;
    bool constant_propagation;
} lr_ir_pipeline_t;

typedef int (*lr_write_cb)(void *user, const char *data, size_t len);

typedef struct lr_owned_module {
    lr_module_t *module;
    struct lr_owned_module *next;
} lr_owned_module_t;

struct lr_compile_session {
    lr_compile_config_t cfg;
    lr_jit_t *jit;
    lr_module_t *module;
    lr_func_t *cur_func;
    lr_block_t *cur_block;
    lr_block_t **blocks;
    uint8_t *sealed;
    uint32_t block_count;
    uint32_t block_cap;
    lr_owned_module_t *owned_modules;
};

static void err_clear(lr_compile_error_t *err) {
    if (!err)
        return;
    err->code = LR_COMPILE_OK;
    err->message[0] = '\0';
}

static void err_set(lr_compile_error_t *err, lr_compile_error_code_t code,
                    const char *fmt, ...) {
    va_list args;
    if (!err)
        return;
    err->code = code;
    va_start(args, fmt);
    (void)vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);
}

static lr_module_t *compile_module_create(void) {
    lr_arena_t *arena = lr_arena_create(0);
    lr_module_t *m = NULL;
    if (!arena)
        return NULL;
    m = lr_module_create(arena);
    if (!m) {
        lr_arena_destroy(arena);
        return NULL;
    }
    return m;
}

static bool is_terminator(lr_opcode_t op) {
    switch (op) {
    case LR_OP_RET:
    case LR_OP_RET_VOID:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_UNREACHABLE:
        return true;
    default:
        return false;
    }
}

static bool opcode_has_dest(lr_opcode_t op, lr_type_t *type) {
    switch (op) {
    case LR_OP_RET:
    case LR_OP_RET_VOID:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_UNREACHABLE:
    case LR_OP_STORE:
        return false;
    case LR_OP_CALL:
        return type && type->kind != LR_TYPE_VOID;
    default:
        return true;
    }
}

static lr_operand_t desc_to_op(const lr_operand_desc_t *d) {
    lr_operand_t op;
    memset(&op, 0, sizeof(op));
    if (!d) {
        op.kind = LR_VAL_UNDEF;
        return op;
    }
    op.kind = (lr_operand_kind_t)d->kind;
    op.type = d->type;
    switch (d->kind) {
    case LR_OP_KIND_VREG:
        op.vreg = d->vreg;
        break;
    case LR_OP_KIND_IMM_I64:
        op.imm_i64 = d->imm_i64;
        break;
    case LR_OP_KIND_IMM_F64:
        op.imm_f64 = d->imm_f64;
        break;
    case LR_OP_KIND_BLOCK:
        op.block_id = d->block_id;
        break;
    case LR_OP_KIND_GLOBAL:
        op.global_id = d->global_id;
        break;
    default:
        break;
    }
    return op;
}

static int ensure_block_capacity(lr_compile_session_t *s, uint32_t need) {
    lr_block_t **new_blocks = NULL;
    uint8_t *new_sealed = NULL;
    uint32_t new_cap = 0;
    if (need <= s->block_cap)
        return 0;

    new_cap = s->block_cap == 0 ? 8u : s->block_cap;
    while (new_cap < need)
        new_cap *= 2u;

    new_blocks = (lr_block_t **)calloc(new_cap, sizeof(*new_blocks));
    new_sealed = (uint8_t *)calloc(new_cap, sizeof(*new_sealed));
    if (!new_blocks || !new_sealed) {
        free(new_blocks);
        free(new_sealed);
        return -1;
    }

    if (s->block_cap > 0) {
        memcpy(new_blocks, s->blocks, sizeof(*new_blocks) * s->block_cap);
        memcpy(new_sealed, s->sealed, sizeof(*new_sealed) * s->block_cap);
    }
    free(s->blocks);
    free(s->sealed);
    s->blocks = new_blocks;
    s->sealed = new_sealed;
    s->block_cap = new_cap;
    return 0;
}

static int ensure_block(lr_compile_session_t *s, uint32_t block_id,
                        lr_compile_error_t *err) {
    char name_buf[32];
    if (!s || !s->cur_func || !s->module) {
        err_set(err, LR_COMPILE_ERR_STATE, "no active function");
        return -1;
    }
    if (ensure_block_capacity(s, block_id + 1u) != 0) {
        err_set(err, LR_COMPILE_ERR_BACKEND, "block table allocation failed");
        return -1;
    }
    while (s->block_count <= block_id) {
        uint32_t next_id = s->block_count;
        lr_block_t *b = NULL;
        (void)snprintf(name_buf, sizeof(name_buf), "b%u", next_id);
        b = lr_block_create(s->cur_func, s->module->arena, name_buf);
        if (!b) {
            err_set(err, LR_COMPILE_ERR_BACKEND, "block creation failed");
            return -1;
        }
        if (b->id != next_id) {
            err_set(err, LR_COMPILE_ERR_STATE, "non-dense block id allocation");
            return -1;
        }
        s->blocks[next_id] = b;
        s->sealed[next_id] = 0;
        s->block_count++;
    }
    return 0;
}

static int validate_function_blocks(lr_compile_session_t *s,
                                    lr_compile_error_t *err) {
    uint32_t i;
    if (!s || !s->cur_func)
        return -1;
    for (i = 0; i < s->block_count; i++) {
        lr_block_t *b = s->blocks[i];
        if (!b || !b->last || !is_terminator(b->last->op)) {
            err_set(err, LR_COMPILE_ERR_STATE, "block %u is not terminated", i);
            return -1;
        }
        s->sealed[i] = 1;
    }
    return 0;
}

static int own_module(lr_compile_session_t *s, lr_module_t *m) {
    lr_owned_module_t *node = NULL;
    if (!s || !m)
        return -1;
    node = (lr_owned_module_t *)calloc(1, sizeof(*node));
    if (!node)
        return -1;
    node->module = m;
    node->next = s->owned_modules;
    s->owned_modules = node;
    return 0;
}

static void free_owned_modules(lr_compile_session_t *s) {
    lr_owned_module_t *it = NULL;
    if (!s)
        return;
    it = s->owned_modules;
    while (it) {
        lr_owned_module_t *next = it->next;
        lr_module_free(it->module);
        free(it);
        it = next;
    }
    s->owned_modules = NULL;
}

static int compile_current_function(lr_compile_session_t *s,
                                    lr_symbol_handle_t *out_symbol,
                                    lr_compile_error_t *err) {
    lr_func_t *f = NULL;
    lr_func_t **toggled = NULL;
    uint32_t toggled_count = 0;
    uint32_t toggled_cap = 0;
    void *addr = NULL;
    int rc;
    bool restore_toggled = false;

    if (!s || !s->jit || !s->module || !s->cur_func || !s->cur_func->name) {
        err_set(err, LR_COMPILE_ERR_STATE, "no active function");
        return -1;
    }

    if (lr_func_finalize(s->cur_func, s->module->arena) != 0) {
        err_set(err, LR_COMPILE_ERR_BACKEND, "function finalization failed");
        return -1;
    }

    if (s->cfg.strategy == LR_COMPILE_STRATEGY_IR_MODE)
        restore_toggled = true;

    for (f = s->module->first_func; f; f = f->next) {
        if (f == s->cur_func || f->is_decl)
            continue;
        if (toggled_count == toggled_cap) {
            uint32_t new_cap = toggled_cap == 0 ? 8u : (toggled_cap * 2u);
            lr_func_t **new_list = (lr_func_t **)realloc(
                toggled, sizeof(*toggled) * new_cap
            );
            if (!new_list) {
                free(toggled);
                err_set(err, LR_COMPILE_ERR_BACKEND,
                        "temporary function list allocation failed");
                return -1;
            }
            toggled = new_list;
            toggled_cap = new_cap;
        }
        toggled[toggled_count++] = f;
        f->is_decl = true;
    }

    rc = lr_jit_add_module(s->jit, s->module);
    if (rc != 0) {
        uint32_t i;
        for (i = 0; i < toggled_count; i++)
            toggled[i]->is_decl = false;
        free(toggled);
        err_set(err, LR_COMPILE_ERR_BACKEND, "module code generation failed");
        return -1;
    }

    if (restore_toggled) {
        uint32_t i;
        for (i = 0; i < toggled_count; i++)
            toggled[i]->is_decl = false;
    } else {
        s->cur_func->is_decl = true;
    }
    free(toggled);

    addr = lr_jit_get_function(s->jit, s->cur_func->name);
    if (!addr) {
        err_set(err, LR_COMPILE_ERR_NOT_FOUND,
                "compiled symbol lookup failed: %s", s->cur_func->name);
        return -1;
    }

    if (out_symbol) {
        out_symbol->name = s->cur_func->name;
        out_symbol->addr = addr;
    }
    return 0;
}

lr_compile_session_t *lr_compile_begin(const lr_compile_config_t *cfg,
                                       lr_compile_error_t *err) {
    lr_compile_session_t *s = NULL;
    lr_compile_config_t defaults;
    err_clear(err);

    defaults.strategy = LR_COMPILE_STRATEGY_DIRECT_PASS;
    defaults.target_name = NULL;
    defaults.enable_local_peephole = false;
    defaults.enable_ir_pipeline = false;

    if (cfg && cfg->strategy != LR_COMPILE_STRATEGY_DIRECT_PASS &&
        cfg->strategy != LR_COMPILE_STRATEGY_IR_MODE) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT,
                "invalid compile strategy");
        return NULL;
    }

    s = (lr_compile_session_t *)calloc(1, sizeof(*s));
    if (!s) {
        err_set(err, LR_COMPILE_ERR_BACKEND, "session allocation failed");
        return NULL;
    }
    s->cfg = cfg ? *cfg : defaults;

    s->module = compile_module_create();
    if (!s->module) {
        free(s);
        err_set(err, LR_COMPILE_ERR_BACKEND, "module allocation failed");
        return NULL;
    }

    if (s->cfg.target_name && s->cfg.target_name[0])
        s->jit = lr_jit_create_for_target(s->cfg.target_name);
    else
        s->jit = lr_jit_create();
    if (!s->jit) {
        lr_module_free(s->module);
        free(s);
        err_set(err, LR_COMPILE_ERR_BACKEND, "jit creation failed");
        return NULL;
    }

    return s;
}

void lr_compile_end(lr_compile_session_t *s) {
    if (!s)
        return;
    if (s->jit)
        lr_jit_destroy(s->jit);
    if (s->module)
        lr_module_free(s->module);
    free_owned_modules(s);
    free(s->blocks);
    free(s->sealed);
    free(s);
}

int lr_add_symbol(lr_compile_session_t *s, const char *name, void *addr,
                  lr_compile_error_t *err) {
    err_clear(err);
    if (!s || !s->jit || !name || !name[0]) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "invalid symbol input");
        return -1;
    }
    lr_jit_add_symbol(s->jit, name, addr);
    return 0;
}

void *lr_lookup_symbol(lr_compile_session_t *s, const char *name) {
    if (!s || !s->jit || !name || !name[0])
        return NULL;
    return lr_jit_get_function(s->jit, name);
}

lr_type_t *lr_compile_type_void(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_void : NULL;
}

lr_type_t *lr_compile_type_i1(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_i1 : NULL;
}

lr_type_t *lr_compile_type_i8(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_i8 : NULL;
}

lr_type_t *lr_compile_type_i16(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_i16 : NULL;
}

lr_type_t *lr_compile_type_i32(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_i32 : NULL;
}

lr_type_t *lr_compile_type_i64(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_i64 : NULL;
}

lr_type_t *lr_compile_type_float(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_float : NULL;
}

lr_type_t *lr_compile_type_double(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_double : NULL;
}

lr_type_t *lr_compile_type_ptr(lr_compile_session_t *s) {
    return (s && s->module) ? s->module->type_ptr : NULL;
}

lr_type_t *lr_compile_type_array(lr_compile_session_t *s, lr_type_t *elem,
                                 uint64_t count) {
    if (!s || !s->module || !elem)
        return NULL;
    return lr_type_array(s->module->arena, elem, count);
}

lr_type_t *lr_compile_type_struct(lr_compile_session_t *s, lr_type_t **fields,
                                  uint32_t num_fields, bool packed) {
    if (!s || !s->module || (num_fields > 0 && !fields))
        return NULL;
    return lr_type_struct(s->module->arena, fields, num_fields, packed, NULL);
}

lr_type_t *lr_compile_type_func(lr_compile_session_t *s, lr_type_t *ret,
                                lr_type_t **params, uint32_t num_params,
                                bool vararg) {
    if (!s || !s->module || !ret || (num_params > 0 && !params))
        return NULL;
    return lr_type_func(s->module->arena, ret, params, num_params, vararg);
}

int lr_func_begin(lr_compile_session_t *s, const lr_function_spec_t *spec,
                  lr_compile_error_t *err) {
    err_clear(err);
    if (!s || !s->module || !spec || !spec->name || !spec->name[0]) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT,
                "invalid function declaration");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "function already active");
        return -1;
    }

    s->cur_func = lr_func_create(
        s->module, spec->name,
        spec->ret_type ? spec->ret_type : s->module->type_void,
        spec->param_types, spec->num_params, spec->vararg
    );
    if (!s->cur_func) {
        err_set(err, LR_COMPILE_ERR_BACKEND, "function creation failed");
        return -1;
    }

    s->cur_block = NULL;
    s->block_count = 0;
    if (ensure_block(s, 0, err) != 0)
        return -1;
    s->cur_block = s->blocks[0];
    return 0;
}

int lr_block_begin(lr_compile_session_t *s, lr_block_id_t block,
                   lr_compile_error_t *err) {
    err_clear(err);
    if (!s || !s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "no active function");
        return -1;
    }
    if (ensure_block(s, block, err) != 0)
        return -1;
    s->cur_block = s->blocks[block];
    return 0;
}

int lr_emit(lr_compile_session_t *s, const lr_inst_desc_t *inst,
            lr_compile_error_t *err) {
    lr_operand_t *ops = NULL;
    lr_inst_t *out = NULL;
    lr_type_t *itype = NULL;
    uint32_t i;
    uint32_t dest = 0;

    err_clear(err);
    if (!s || !s->module || !s->cur_func || !s->cur_block || !inst) {
        err_set(err, LR_COMPILE_ERR_STATE, "no active block");
        return -1;
    }

    if (inst->num_operands > 0 && !inst->operands) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "null operand list");
        return -1;
    }
    if (inst->num_indices > 0 && !inst->indices) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "null index list");
        return -1;
    }

    if (inst->num_operands > 0) {
        ops = lr_arena_array(s->module->arena, lr_operand_t, inst->num_operands);
        if (!ops) {
            err_set(err, LR_COMPILE_ERR_BACKEND, "operand allocation failed");
            return -1;
        }
        for (i = 0; i < inst->num_operands; i++)
            ops[i] = desc_to_op(&inst->operands[i]);
    }

    if (inst->op == LR_OP_GEP && inst->num_operands > 1) {
        for (i = 1; i < inst->num_operands; i++) {
            ops[i] = lr_canonicalize_gep_index(
                s->module, s->cur_block, s->cur_func, ops[i]
            );
        }
    }

    itype = inst->type;
    if (!itype) {
        if (inst->op == LR_OP_ICMP || inst->op == LR_OP_FCMP)
            itype = s->module->type_i1;
        else if (is_terminator(inst->op) || inst->op == LR_OP_STORE)
            itype = s->module->type_void;
    }
    if (!itype && inst->op != LR_OP_CALL) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "instruction type missing");
        return -1;
    }

    if (opcode_has_dest(inst->op, itype)) {
        dest = inst->dest;
        if (dest == 0) {
            dest = lr_vreg_new(s->cur_func);
        } else if (dest >= s->cur_func->next_vreg) {
            s->cur_func->next_vreg = dest + 1u;
        }
    } else {
        dest = 0;
    }

    out = lr_inst_create(s->module->arena, inst->op, itype, dest, ops,
                         inst->num_operands);
    if (!out) {
        err_set(err, LR_COMPILE_ERR_BACKEND, "instruction allocation failed");
        return -1;
    }

    if (inst->op == LR_OP_ICMP)
        out->icmp_pred = (lr_icmp_pred_t)inst->icmp_pred;
    if (inst->op == LR_OP_FCMP)
        out->fcmp_pred = (lr_fcmp_pred_t)inst->fcmp_pred;
    if (inst->op == LR_OP_CALL) {
        out->call_external_abi = inst->call_external_abi;
        out->call_vararg = inst->call_vararg;
    }
    if ((inst->op == LR_OP_EXTRACTVALUE || inst->op == LR_OP_INSERTVALUE) &&
        inst->num_indices > 0) {
        out->indices = lr_arena_array(s->module->arena, uint32_t,
                                      inst->num_indices);
        if (!out->indices) {
            err_set(err, LR_COMPILE_ERR_BACKEND, "index allocation failed");
            return -1;
        }
        memcpy(out->indices, inst->indices, sizeof(uint32_t) * inst->num_indices);
        out->num_indices = inst->num_indices;
    }

    lr_block_append(s->cur_block, out);
    return 0;
}

int lr_block_seal(lr_compile_session_t *s, lr_block_id_t block,
                  lr_compile_error_t *err) {
    err_clear(err);
    if (!s || !s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "no active function");
        return -1;
    }
    if (block >= s->block_count || !s->blocks[block]) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "unknown block %u", block);
        return -1;
    }
    if (!s->blocks[block]->last || !is_terminator(s->blocks[block]->last->op)) {
        err_set(err, LR_COMPILE_ERR_STATE, "block %u is not terminated", block);
        return -1;
    }
    s->sealed[block] = 1;
    return 0;
}

int lr_func_end(lr_compile_session_t *s, lr_symbol_handle_t *out_symbol,
                lr_compile_error_t *err) {
    int rc;
    err_clear(err);
    if (!s || !s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "no active function");
        return -1;
    }
    if (validate_function_blocks(s, err) != 0)
        return -1;

    rc = compile_current_function(s, out_symbol, err);
    if (rc != 0)
        return -1;

    s->cur_func = NULL;
    s->cur_block = NULL;
    s->block_count = 0;
    return 0;
}

int lr_ir_optimize(lr_compile_session_t *s, const lr_ir_pipeline_t *pipe,
                   lr_compile_error_t *err) {
    (void)pipe;
    err_clear(err);
    if (!s) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "null session");
        return -1;
    }
    if (s->cfg.strategy != LR_COMPILE_STRATEGY_IR_MODE) {
        err_set(err, LR_COMPILE_ERR_MODE_CONFLICT,
                "IR optimization requires IR strategy");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "cannot optimize during active function");
        return -1;
    }
    return 0;
}

int lr_ir_print(lr_compile_session_t *s, lr_write_cb cb, void *user,
                lr_compile_error_t *err) {
    FILE *tmp = NULL;
    char buf[4096];
    size_t nread = 0;
    err_clear(err);

    if (!s || !cb) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "invalid print arguments");
        return -1;
    }
    if (s->cfg.strategy != LR_COMPILE_STRATEGY_IR_MODE) {
        err_set(err, LR_COMPILE_ERR_MODE_CONFLICT,
                "IR printing requires IR strategy");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "cannot print during active function");
        return -1;
    }

    tmp = tmpfile();
    if (!tmp) {
        err_set(err, LR_COMPILE_ERR_BACKEND, "tmpfile allocation failed");
        return -1;
    }

    lr_module_dump(s->module, tmp);
    (void)fflush(tmp);
    (void)rewind(tmp);

    while ((nread = fread(buf, 1, sizeof(buf), tmp)) > 0) {
        if (cb(user, buf, nread) != 0) {
            (void)fclose(tmp);
            err_set(err, LR_COMPILE_ERR_STATE, "writer callback aborted");
            return -1;
        }
    }
    (void)fclose(tmp);
    return 0;
}

int lr_compile_ll(lr_compile_session_t *s, const char *src, size_t len,
                  lr_symbol_handle_t *out_last_symbol,
                  lr_compile_error_t *err) {
    char parse_err[256];
    lr_module_t *m = NULL;
    lr_func_t *last = NULL;
    int rc;

    err_clear(err);
    if (out_last_symbol) {
        out_last_symbol->name = NULL;
        out_last_symbol->addr = NULL;
    }
    if (!s || !s->jit || !src || len == 0) {
        err_set(err, LR_COMPILE_ERR_INVALID_ARGUMENT, "invalid ll input");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, LR_COMPILE_ERR_STATE, "cannot parse ll during active function");
        return -1;
    }

    parse_err[0] = '\0';
    m = lr_parse_ll(src, len, parse_err, sizeof(parse_err));
    if (!m) {
        err_set(err, LR_COMPILE_ERR_PARSE, "ll parse failed: %s",
                parse_err[0] ? parse_err : "unknown error");
        return -1;
    }

    rc = lr_jit_add_module(s->jit, m);
    if (rc != 0) {
        lr_module_free(m);
        err_set(err, LR_COMPILE_ERR_BACKEND, "ll module code generation failed");
        return -1;
    }

    if (own_module(s, m) != 0) {
        lr_module_free(m);
        err_set(err, LR_COMPILE_ERR_BACKEND, "module ownership registration failed");
        return -1;
    }

    for (last = m->first_func; last; last = last->next) {
        if (!last->is_decl && last->name && last->name[0]) {
            if (out_last_symbol) {
                out_last_symbol->name = last->name;
                out_last_symbol->addr = lr_jit_get_function(s->jit, last->name);
            }
        }
    }

    if (out_last_symbol && out_last_symbol->name &&
        !out_last_symbol->addr) {
        err_set(err, LR_COMPILE_ERR_NOT_FOUND,
                "compiled symbol lookup failed: %s", out_last_symbol->name);
        return -1;
    }
    return 0;
}
