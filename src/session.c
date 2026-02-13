#include "arena.h"
#include "bc_decode.h"
#include "ir.h"
#include "jit.h"
#include "liric.h"
#include "module_emit.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Session mode mirrors the public lr_session_mode_t. */
typedef enum session_mode {
    SESSION_MODE_DIRECT = 0,
    SESSION_MODE_IR = 1,
} session_mode_t;

/* Session config mirrors the public lr_session_config_t. */
typedef struct session_config {
    session_mode_t mode;
    const char *target;
} session_config_t;

/* Error mirrors the public lr_error_t. */
typedef struct session_error {
    int code;
    char msg[256];
} session_error_t;

enum {
    S_OK = 0,
    S_ERR_ARGUMENT = 1,
    S_ERR_STATE = 2,
    S_ERR_MODE = 3,
    S_ERR_NOT_FOUND = 4,
    S_ERR_BACKEND = 5,
    S_ERR_PARSE = 6,
};

/* Instruction descriptor mirrors the public lr_inst_desc_t. */
typedef struct session_inst_desc {
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
    uint32_t call_fixed_args;
} session_inst_desc_t;

typedef struct lr_owned_module {
    lr_module_t *module;
    struct lr_owned_module *next;
} lr_owned_module_t;

typedef struct session_phi_copy_entry {
    uint32_t pred_block_id;
    lr_phi_copy_desc_t copy;
} session_phi_copy_entry_t;

struct lr_session {
    session_config_t cfg;
    lr_jit_t *jit;
    lr_module_t *module;
    lr_func_t *cur_func;
    lr_block_t *cur_block;
    lr_block_t **blocks;
    bool *block_seen;
    bool *block_terminated;
    uint32_t block_count;
    uint32_t block_cap;
    lr_owned_module_t *owned_modules;
    session_phi_copy_entry_t *phi_copies;
    uint32_t phi_copy_count;
    uint32_t phi_copy_cap;
    void *compile_ctx;
    size_t compile_start;
    bool compile_active;
    uint32_t emitted_count;
};

static int ensure_block(struct lr_session *s, uint32_t block_id,
                        session_error_t *err);

/* ---- Error helpers ----------------------------------------------------- */

static void err_clear(session_error_t *err) {
    if (!err)
        return;
    err->code = S_OK;
    err->msg[0] = '\0';
}

static void err_set(session_error_t *err, int code, const char *fmt, ...) {
    va_list args;
    if (!err)
        return;
    err->code = code;
    va_start(args, fmt);
    (void)vsnprintf(err->msg, sizeof(err->msg), fmt, args);
    va_end(args);
}

/* ---- Internal helpers -------------------------------------------------- */

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

static lr_operand_t operand_desc_to_operand(const lr_operand_desc_t *d) {
    lr_operand_t op;
    memset(&op, 0, sizeof(op));
    if (!d) {
        op.kind = LR_VAL_UNDEF;
        return op;
    }
    op.kind = (lr_operand_kind_t)d->kind;
    op.type = d->type;
    op.global_offset = d->global_offset;
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

static size_t align_up_size(size_t value, size_t alignment) {
    return (value + (alignment - 1u)) & ~(alignment - 1u);
}

static int ensure_phi_copy_capacity(struct lr_session *s, uint32_t need) {
    session_phi_copy_entry_t *new_entries = NULL;
    uint32_t new_cap = 0;
    if (!s)
        return -1;
    if (need <= s->phi_copy_cap)
        return 0;
    new_cap = s->phi_copy_cap == 0 ? 8u : s->phi_copy_cap;
    while (new_cap < need)
        new_cap *= 2u;
    new_entries = (session_phi_copy_entry_t *)calloc(new_cap, sizeof(*new_entries));
    if (!new_entries)
        return -1;
    if (s->phi_copy_count > 0) {
        memcpy(new_entries, s->phi_copies,
               sizeof(*new_entries) * s->phi_copy_count);
    }
    free(s->phi_copies);
    s->phi_copies = new_entries;
    s->phi_copy_cap = new_cap;
    return 0;
}

static void reset_phi_copies(struct lr_session *s) {
    if (!s)
        return;
    s->phi_copy_count = 0;
}

static int ensure_block_capacity(struct lr_session *s, uint32_t need) {
    lr_block_t **new_blocks = NULL;
    bool *new_seen = NULL;
    bool *new_terminated = NULL;
    uint32_t new_cap = 0;
    if (need <= s->block_cap)
        return 0;
    new_cap = s->block_cap == 0 ? 8u : s->block_cap;
    while (new_cap < need)
        new_cap *= 2u;
    new_blocks = (lr_block_t **)calloc(new_cap, sizeof(*new_blocks));
    new_seen = (bool *)calloc(new_cap, sizeof(*new_seen));
    new_terminated = (bool *)calloc(new_cap, sizeof(*new_terminated));
    if (!new_blocks || !new_seen || !new_terminated) {
        free(new_blocks);
        free(new_seen);
        free(new_terminated);
        return -1;
    }
    if (s->block_cap > 0) {
        memcpy(new_blocks, s->blocks, sizeof(*new_blocks) * s->block_cap);
        memcpy(new_seen, s->block_seen, sizeof(*new_seen) * s->block_cap);
        memcpy(new_terminated, s->block_terminated,
               sizeof(*new_terminated) * s->block_cap);
    }
    free(s->blocks);
    free(s->block_seen);
    free(s->block_terminated);
    s->blocks = new_blocks;
    s->block_seen = new_seen;
    s->block_terminated = new_terminated;
    s->block_cap = new_cap;
    return 0;
}

static void reset_block_tracking(struct lr_session *s) {
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->block_count; i++) {
        s->block_seen[i] = false;
        s->block_terminated[i] = false;
    }
}

static bool direct_mode_enabled(const struct lr_session *s) {
    return s &&
           s->cfg.mode == SESSION_MODE_DIRECT &&
           s->jit &&
           s->jit->target &&
           lr_target_can_compile(s->jit->target, s->jit->mode);
}

static int begin_direct_compile(struct lr_session *s, session_error_t *err) {
    lr_compile_func_meta_t meta;
    int rc;

    if (!direct_mode_enabled(s) || !s->cur_func)
        return 0;

    memset(&meta, 0, sizeof(meta));
    meta.func = s->cur_func;
    meta.ret_type = s->cur_func->ret_type;
    meta.param_types = s->cur_func->param_types;
    meta.num_params = s->cur_func->num_params;
    meta.vararg = s->cur_func->vararg;
    meta.num_blocks = s->block_count;
    meta.next_vreg = s->cur_func->next_vreg;
    meta.mode = s->jit->mode;

    s->compile_start = align_up_size(s->jit->code_size, 16u);
    if (s->compile_start >= s->jit->code_cap) {
        err_set(err, S_ERR_BACKEND, "jit code buffer exhausted");
        return -1;
    }

    rc = s->jit->target->compile_begin(
        &s->compile_ctx, &meta, s->module,
        s->jit->code_buf + s->compile_start,
        s->jit->code_cap - s->compile_start,
        s->jit->arena
    );
    if (rc != 0 || !s->compile_ctx) {
        err_set(err, S_ERR_BACKEND, "backend compile begin failed");
        s->compile_ctx = NULL;
        return -1;
    }

    s->compile_active = true;
    return 0;
}

static int finish_direct_compile(struct lr_session *s, void **out_addr,
                                 session_error_t *err) {
    size_t code_len = 0;
    int rc;
    bool opened_update = false;

    if (!s || !s->cur_func || !s->jit || !s->compile_active || !s->compile_ctx) {
        err_set(err, S_ERR_STATE, "no active direct compile context");
        return -1;
    }

    if (!s->jit->update_active) {
        lr_jit_begin_update(s->jit);
        opened_update = s->jit->update_active;
    }
    if (!s->jit->update_active) {
        err_set(err, S_ERR_BACKEND, "jit update transition failed");
        return -1;
    }

    rc = s->jit->target->compile_end(s->compile_ctx, &code_len);
    s->compile_ctx = NULL;
    s->compile_active = false;
    if (rc != 0) {
        err_set(err, S_ERR_BACKEND, "backend compile end failed");
        if (opened_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }

    if (s->compile_start + code_len > s->jit->code_cap) {
        err_set(err, S_ERR_BACKEND, "jit code buffer overflow");
        if (opened_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }

    s->jit->code_size = s->compile_start + code_len;
    if (s->jit->update_active && code_len > 0)
        s->jit->update_dirty = true;

    lr_jit_add_symbol(s->jit, s->cur_func->name,
                      s->jit->code_buf + s->compile_start);
    s->cur_func->is_decl = true;
    if (out_addr)
        *out_addr = s->jit->code_buf + s->compile_start;

    if (opened_update && s->jit->update_active)
        lr_jit_end_update(s->jit);
    return 0;
}

static int emit_ir_instruction(struct lr_session *s, const session_inst_desc_t *inst,
                               session_error_t *err, uint32_t *out_dest) {
    lr_operand_t *ops = NULL;
    lr_inst_t *out = NULL;
    uint32_t i;
    if (!s || !s->module || !s->cur_func || !s->cur_block || !inst) {
        err_set(err, S_ERR_STATE, "no active block");
        return -1;
    }
    if (inst->num_operands > 0 && !inst->operands) {
        err_set(err, S_ERR_ARGUMENT, "null operand list");
        return -1;
    }
    if (inst->num_indices > 0 && !inst->indices) {
        err_set(err, S_ERR_ARGUMENT, "null index list");
        return -1;
    }
    if (inst->num_operands > 0) {
        ops = lr_arena_array(s->module->arena, lr_operand_t,
                             inst->num_operands);
        if (!ops) {
            err_set(err, S_ERR_BACKEND, "operand allocation failed");
            return -1;
        }
        for (i = 0; i < inst->num_operands; i++)
            ops[i] = operand_desc_to_operand(&inst->operands[i]);
    }

    if (inst->op == LR_OP_GEP && inst->num_operands > 1) {
        for (i = 1; i < inst->num_operands; i++) {
            ops[i] = lr_canonicalize_gep_index(
                s->module, s->cur_block, s->cur_func, ops[i]
            );
        }
    }

    out = lr_inst_create(s->module->arena, inst->op, inst->type, inst->dest, ops,
                         inst->num_operands);
    if (!out) {
        err_set(err, S_ERR_BACKEND, "instruction allocation failed");
        return -1;
    }

    if (inst->op == LR_OP_ICMP)
        out->icmp_pred = (lr_icmp_pred_t)inst->icmp_pred;
    if (inst->op == LR_OP_FCMP)
        out->fcmp_pred = (lr_fcmp_pred_t)inst->fcmp_pred;
    if (inst->op == LR_OP_CALL) {
        out->call_external_abi = inst->call_external_abi;
        out->call_vararg = inst->call_vararg;
        out->call_fixed_args = inst->call_fixed_args;
    }
    if ((inst->op == LR_OP_EXTRACTVALUE || inst->op == LR_OP_INSERTVALUE) &&
        inst->num_indices > 0) {
        out->indices = lr_arena_array(s->module->arena, uint32_t,
                                      inst->num_indices);
        if (!out->indices) {
            err_set(err, S_ERR_BACKEND, "index allocation failed");
            return -1;
        }
        memcpy(out->indices, inst->indices,
               sizeof(uint32_t) * inst->num_indices);
        out->num_indices = inst->num_indices;
    }

    lr_block_append(s->cur_block, out);
    if (out_dest)
        *out_dest = inst->dest;
    return 0;
}

static int validate_block_termination(struct lr_session *s,
                                      session_error_t *err) {
    uint32_t i;
    if (!s || !s->cur_func)
        return -1;
    if (s->block_count == 0) {
        err_set(err, S_ERR_STATE, "block 0 is not terminated");
        return -1;
    }
    for (i = 0; i < s->block_count; i++) {
        if (!s->block_seen[i] || !s->block_terminated[i]) {
            err_set(err, S_ERR_STATE, "block %u is not terminated", i);
            return -1;
        }
    }
    return 0;
}

static void finish_function_state(struct lr_session *s) {
    if (!s)
        return;
    reset_block_tracking(s);
    reset_phi_copies(s);
    s->cur_func = NULL;
    s->cur_block = NULL;
    s->block_count = 0;
    s->compile_ctx = NULL;
    s->compile_start = 0;
    s->compile_active = false;
    s->emitted_count = 0;
}

static int append_phi_copy(struct lr_session *s, uint32_t pred_block_id,
                           const lr_phi_copy_desc_t *copy,
                           session_error_t *err) {
    if (!s || !copy)
        return -1;
    if (ensure_phi_copy_capacity(s, s->phi_copy_count + 1u) != 0) {
        err_set(err, S_ERR_BACKEND, "phi copy allocation failed");
        return -1;
    }
    s->phi_copies[s->phi_copy_count].pred_block_id = pred_block_id;
    s->phi_copies[s->phi_copy_count].copy = *copy;
    s->phi_copy_count++;
    return 0;
}

static int ensure_block(struct lr_session *s, uint32_t block_id,
                         session_error_t *err) {
    char name_buf[32];
    if (!s || !s->cur_func || !s->module) {
        err_set(err, S_ERR_STATE, "no active function");
        return -1;
    }
    if (ensure_block_capacity(s, block_id + 1u) != 0) {
        err_set(err, S_ERR_BACKEND, "block table allocation failed");
        return -1;
    }
    while (s->block_count <= block_id) {
        uint32_t next_id = s->block_count;
        lr_block_t *b = NULL;
        (void)snprintf(name_buf, sizeof(name_buf), "b%u", next_id);
        b = lr_block_create(s->cur_func, s->module->arena, name_buf);
        if (!b) {
            err_set(err, S_ERR_BACKEND, "block creation failed");
            return -1;
        }
        if (b->id != next_id) {
            err_set(err, S_ERR_STATE, "non-dense block id allocation");
            return -1;
        }
        s->blocks[next_id] = b;
        s->block_count++;
    }
    return 0;
}

static lr_global_t *find_global_by_id(struct lr_session *s, uint32_t id) {
    lr_global_t *g;
    for (g = s->module->first_global; g; g = g->next) {
        if (g->id == id)
            return g;
    }
    return NULL;
}

static int compile_current_function(struct lr_session *s, void **out_addr,
                                     session_error_t *err) {
    lr_func_t *f = NULL;
    lr_func_t **toggled = NULL;
    uint32_t toggled_count = 0;
    uint32_t toggled_cap = 0;
    void *addr = NULL;
    int rc;
    bool restore_toggled = false;

    if (!s || !s->jit || !s->module || !s->cur_func || !s->cur_func->name) {
        err_set(err, S_ERR_STATE, "no active function");
        return -1;
    }

    if (lr_func_finalize(s->cur_func, s->module->arena) != 0) {
        err_set(err, S_ERR_BACKEND, "function finalization failed");
        return -1;
    }

    /* IR mode without address request: finalize only, skip JIT compilation.
       This allows building modules for object file emission without resolving
       all external symbols at IR construction time. */
    if (s->cfg.mode == SESSION_MODE_IR && !out_addr)
        return 0;

    if (s->cfg.mode == SESSION_MODE_IR)
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
                err_set(err, S_ERR_BACKEND, "toggle list allocation failed");
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
        err_set(err, S_ERR_BACKEND, "module code generation failed");
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
        err_set(err, S_ERR_NOT_FOUND,
                "compiled symbol lookup failed: %s", s->cur_func->name);
        return -1;
    }

    if (out_addr)
        *out_addr = addr;
    return 0;
}

/* ---- Lifecycle --------------------------------------------------------- */

struct lr_session *lr_session_create(const void *cfg_ptr,
                                      session_error_t *err) {
    const session_config_t *cfg = (const session_config_t *)cfg_ptr;
    struct lr_session *s = NULL;
    lr_arena_t *arena = NULL;
    err_clear(err);

    if (cfg && cfg->mode != SESSION_MODE_DIRECT &&
        cfg->mode != SESSION_MODE_IR) {
        err_set(err, S_ERR_ARGUMENT, "invalid session mode");
        return NULL;
    }

    s = (struct lr_session *)calloc(1, sizeof(*s));
    if (!s) {
        err_set(err, S_ERR_BACKEND, "session allocation failed");
        return NULL;
    }

    if (cfg) {
        s->cfg.mode = cfg->mode;
        s->cfg.target = cfg->target;
    }

    arena = lr_arena_create(0);
    if (!arena) {
        free(s);
        err_set(err, S_ERR_BACKEND, "arena allocation failed");
        return NULL;
    }

    s->module = lr_module_create(arena);
    if (!s->module) {
        lr_arena_destroy(arena);
        free(s);
        err_set(err, S_ERR_BACKEND, "module allocation failed");
        return NULL;
    }

    if (s->cfg.target && s->cfg.target[0])
        s->jit = lr_jit_create_for_target(s->cfg.target);
    else
        s->jit = lr_jit_create();
    if (!s->jit) {
        lr_module_free(s->module);
        free(s);
        err_set(err, S_ERR_BACKEND, "jit creation failed");
        return NULL;
    }

    return s;
}

void lr_session_destroy(struct lr_session *s) {
    lr_owned_module_t *it = NULL;
    if (!s)
        return;
    if (s->jit)
        lr_jit_destroy(s->jit);
    if (s->module)
        lr_module_free(s->module);
    it = s->owned_modules;
    while (it) {
        lr_owned_module_t *next = it->next;
        lr_module_free(it->module);
        free(it);
        it = next;
    }
    free(s->phi_copies);
    free(s->block_terminated);
    free(s->block_seen);
    free(s->blocks);
    free(s);
}

/* ---- Symbols ----------------------------------------------------------- */

void lr_session_add_symbol(struct lr_session *s, const char *name, void *addr) {
    if (!s || !s->jit || !name || !name[0])
        return;
    lr_jit_add_symbol(s->jit, name, addr);
}

void *lr_session_lookup(struct lr_session *s, const char *name) {
    if (!s || !s->jit || !name || !name[0])
        return NULL;
    return lr_jit_get_function(s->jit, name);
}

/* ---- Types (session-scoped singletons) --------------------------------- */

lr_type_t *lr_type_void_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_void : NULL;
}

lr_type_t *lr_type_i1_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_i1 : NULL;
}

lr_type_t *lr_type_i8_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_i8 : NULL;
}

lr_type_t *lr_type_i16_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_i16 : NULL;
}

lr_type_t *lr_type_i32_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_i32 : NULL;
}

lr_type_t *lr_type_i64_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_i64 : NULL;
}

lr_type_t *lr_type_f32_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_float : NULL;
}

lr_type_t *lr_type_f64_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_double : NULL;
}

lr_type_t *lr_type_ptr_s(struct lr_session *s) {
    return (s && s->module) ? s->module->type_ptr : NULL;
}

lr_type_t *lr_type_array_s(struct lr_session *s, lr_type_t *elem,
                            uint64_t count) {
    if (!s || !s->module || !elem)
        return NULL;
    return lr_type_array(s->module->arena, elem, count);
}

lr_type_t *lr_type_struct_s(struct lr_session *s, lr_type_t **fields,
                             uint32_t n, bool packed) {
    if (!s || !s->module || (n > 0 && !fields))
        return NULL;
    return lr_type_struct(s->module->arena, fields, n, packed, NULL);
}

lr_type_t *lr_type_function_s(struct lr_session *s, lr_type_t *ret,
                               lr_type_t **params, uint32_t n, bool vararg) {
    if (!s || !s->module || !ret || (n > 0 && !params))
        return NULL;
    return lr_type_func(s->module->arena, ret, params, n, vararg);
}

/* ---- Globals ----------------------------------------------------------- */

uint32_t lr_session_global(struct lr_session *s, const char *name,
                            lr_type_t *type, bool is_const, const void *init,
                            size_t init_size) {
    lr_global_t *g;
    if (!s || !s->module || !name)
        return UINT32_MAX;
    g = lr_global_create(s->module, name, type, is_const);
    if (!g)
        return UINT32_MAX;
    if (init && init_size > 0) {
        g->init_data = lr_arena_alloc(s->module->arena, init_size, 1);
        if (!g->init_data)
            return UINT32_MAX;
        memcpy(g->init_data, init, init_size);
        g->init_size = init_size;
    }
    return g->id;
}

uint32_t lr_session_global_extern(struct lr_session *s, const char *name,
                                   lr_type_t *type) {
    lr_global_t *g;
    if (!s || !s->module || !name)
        return UINT32_MAX;
    g = lr_global_create(s->module, name, type, false);
    if (!g)
        return UINT32_MAX;
    g->is_external = true;
    return g->id;
}

void lr_session_global_reloc(struct lr_session *s, uint32_t id, size_t offset,
                              const char *sym) {
    lr_global_t *g;
    lr_reloc_t *r;
    if (!s || !s->module || !sym)
        return;
    g = find_global_by_id(s, id);
    if (!g)
        return;
    r = lr_arena_new(s->module->arena, lr_reloc_t);
    if (!r)
        return;
    r->offset = offset;
    r->symbol_name = lr_arena_strdup(s->module->arena, sym, strlen(sym));
    r->addend = 0;
    r->next = g->relocs;
    g->relocs = r;
}

uint32_t lr_session_intern(struct lr_session *s, const char *name) {
    if (!s || !s->module || !name)
        return UINT32_MAX;
    return lr_module_intern_symbol(s->module, name);
}

/* ---- Function ---------------------------------------------------------- */

int lr_session_declare(struct lr_session *s, const char *name, lr_type_t *ret,
                        lr_type_t **params, uint32_t n, bool vararg,
                        session_error_t *err) {
    lr_func_t *f;
    err_clear(err);
    if (!s || !s->module || !name || !name[0]) {
        err_set(err, S_ERR_ARGUMENT, "invalid declaration arguments");
        return -1;
    }
    f = lr_func_declare(s->module, name,
                        ret ? ret : s->module->type_void,
                        params, n, vararg);
    if (!f) {
        err_set(err, S_ERR_BACKEND, "function declaration failed");
        return -1;
    }
    return 0;
}

int lr_session_func_begin(struct lr_session *s, const char *name,
                           lr_type_t *ret, lr_type_t **params, uint32_t n,
                           bool vararg, session_error_t *err) {
    err_clear(err);
    if (!s || !s->module || !name || !name[0]) {
        err_set(err, S_ERR_ARGUMENT, "invalid function begin arguments");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, S_ERR_STATE, "function already active");
        return -1;
    }

    s->cur_func = lr_func_create(s->module, name,
                                 ret ? ret : s->module->type_void,
                                 params, n, vararg);
    if (!s->cur_func) {
        err_set(err, S_ERR_BACKEND, "function creation failed");
        return -1;
    }

    s->cur_block = NULL;
    s->block_count = 0;
    reset_phi_copies(s);
    s->compile_ctx = NULL;
    s->compile_start = 0;
    s->compile_active = false;
    s->emitted_count = 0;
    if (begin_direct_compile(s, err) != 0) {
        s->cur_func->is_decl = true;
        finish_function_state(s);
        return -1;
    }
    return 0;
}

uint32_t lr_session_param(struct lr_session *s, uint32_t idx) {
    if (!s || !s->cur_func || idx >= s->cur_func->num_params)
        return UINT32_MAX;
    return s->cur_func->param_vregs[idx];
}

int lr_session_add_phi_copy(struct lr_session *s, uint32_t pred_block_id,
                            const lr_phi_copy_desc_t *copy,
                            session_error_t *err) {
    err_clear(err);
    if (!s || !s->cur_func || !copy) {
        err_set(err, S_ERR_ARGUMENT, "invalid phi copy arguments");
        return -1;
    }
    if (ensure_block(s, pred_block_id, err) != 0)
        return -1;
    if (append_phi_copy(s, pred_block_id, copy, err) != 0)
        return -1;
    return 0;
}

int lr_session_func_end(struct lr_session *s, void **out_addr,
                         session_error_t *err) {
    int rc;
    err_clear(err);
    if (!s || !s->cur_func) {
        err_set(err, S_ERR_STATE, "no active function");
        return -1;
    }
    if (validate_block_termination(s, err) != 0)
        return -1;

    if (s->compile_active)
        rc = finish_direct_compile(s, out_addr, err);
    else
        rc = compile_current_function(s, out_addr, err);
    if (rc != 0)
        return -1;

    finish_function_state(s);
    return 0;
}

/* ---- Blocks ------------------------------------------------------------ */

uint32_t lr_session_block(struct lr_session *s) {
    uint32_t id;
    if (!s || !s->cur_func)
        return UINT32_MAX;
    id = s->block_count;
    if (ensure_block(s, id, NULL) != 0)
        return UINT32_MAX;
    if (s->compile_active && s->emitted_count == 0 && s->block_count > 1) {
        /* Current streaming bridge path is only used for single-block direct emission. */
        s->compile_active = false;
        s->compile_ctx = NULL;
    }
    return id;
}

int lr_session_set_block(struct lr_session *s, uint32_t block_id,
                          session_error_t *err) {
    err_clear(err);
    if (!s || !s->cur_func) {
        err_set(err, S_ERR_STATE, "no active function");
        return -1;
    }
    if (ensure_block(s, block_id, err) != 0)
        return -1;
    s->cur_block = s->blocks[block_id];
    if (s->compile_active) {
        if (!s->compile_ctx || !s->jit->target ||
            !s->jit->target->compile_set_block ||
            s->jit->target->compile_set_block(s->compile_ctx, block_id) != 0) {
            err_set(err, S_ERR_BACKEND, "backend set-block failed");
            return -1;
        }
    }
    return 0;
}

/* ---- Vreg allocation --------------------------------------------------- */

uint32_t lr_session_vreg(struct lr_session *s) {
    if (!s || !s->cur_func)
        return UINT32_MAX;
    return lr_vreg_new(s->cur_func);
}

/* ---- Generic emit ------------------------------------------------------ */

uint32_t lr_session_emit(struct lr_session *s, const void *inst_ptr,
                          session_error_t *err) {
    const session_inst_desc_t *inst = (const session_inst_desc_t *)inst_ptr;
    lr_compile_inst_desc_t compile_desc;
    lr_operand_desc_t *resolved_call_ops = NULL;
    lr_type_t *itype = NULL;
    uint32_t dest = 0;
    session_inst_desc_t normalized;

    err_clear(err);
    if (!s || !s->module || !s->cur_func || !s->cur_block || !inst) {
        err_set(err, S_ERR_STATE, "no active block");
        return 0;
    }

    if (inst->num_operands > 0 && !inst->operands) {
        err_set(err, S_ERR_ARGUMENT, "null operand list");
        return 0;
    }
    if (inst->num_indices > 0 && !inst->indices) {
        err_set(err, S_ERR_ARGUMENT, "null index list");
        return 0;
    }

    itype = inst->type;
    if (!itype) {
        if (inst->op == LR_OP_ICMP || inst->op == LR_OP_FCMP)
            itype = s->module->type_i1;
        else if (is_terminator(inst->op) || inst->op == LR_OP_STORE)
            itype = s->module->type_void;
    }
    if (!itype && inst->op != LR_OP_CALL) {
        err_set(err, S_ERR_ARGUMENT, "instruction type missing");
        return 0;
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

    normalized = *inst;
    normalized.type = itype;
    normalized.dest = dest;

    if (s->compile_active &&
        normalized.op == LR_OP_CALL &&
        normalized.num_operands > 0 &&
        normalized.operands &&
        normalized.operands[0].kind == LR_OP_KIND_GLOBAL) {
        const char *callee_name = lr_module_symbol_name(
            s->module, normalized.operands[0].global_id
        );
        void *callee_addr = NULL;
        if (callee_name && s->jit)
            callee_addr = lr_jit_get_function(s->jit, callee_name);

        if (callee_addr) {
            resolved_call_ops = (lr_operand_desc_t *)calloc(
                normalized.num_operands, sizeof(*resolved_call_ops)
            );
            if (!resolved_call_ops) {
                err_set(err, S_ERR_BACKEND, "call operand allocation failed");
                return 0;
            }
            memcpy(resolved_call_ops, normalized.operands,
                   sizeof(*resolved_call_ops) * normalized.num_operands);
            resolved_call_ops[0].kind = LR_OP_KIND_IMM_I64;
            resolved_call_ops[0].imm_i64 = (int64_t)(intptr_t)callee_addr;
            resolved_call_ops[0].type = s->module->type_ptr;
            resolved_call_ops[0].global_offset = 0;
            normalized.operands = resolved_call_ops;
        } else if (s->emitted_count == 0) {
            /* Fall back before streaming emits so unresolved calls can use normal IR/JIT lowering. */
            s->compile_active = false;
            s->compile_ctx = NULL;
        } else {
            err_set(err, S_ERR_BACKEND,
                    "direct call target unresolved after streaming began");
            return 0;
        }
    }

    if (s->compile_active) {
        if (!s->compile_ctx || !s->jit->target || !s->jit->target->compile_emit) {
            err_set(err, S_ERR_STATE, "no active direct compile context");
            free(resolved_call_ops);
            return 0;
        }
        memset(&compile_desc, 0, sizeof(compile_desc));
        compile_desc.op = normalized.op;
        compile_desc.type = normalized.type;
        compile_desc.dest = normalized.dest;
        compile_desc.operands = normalized.operands;
        compile_desc.num_operands = normalized.num_operands;
        compile_desc.indices = normalized.indices;
        compile_desc.num_indices = normalized.num_indices;
        compile_desc.icmp_pred = normalized.icmp_pred;
        compile_desc.fcmp_pred = normalized.fcmp_pred;
        compile_desc.call_external_abi = normalized.call_external_abi;
        compile_desc.call_vararg = normalized.call_vararg;
        compile_desc.call_fixed_args = normalized.call_fixed_args;
        if (s->jit->target->compile_emit(s->compile_ctx, &compile_desc) != 0) {
            err_set(err, S_ERR_BACKEND, "backend emit failed");
            free(resolved_call_ops);
            return 0;
        }
    } else {
        if (emit_ir_instruction(s, &normalized, err, NULL) != 0) {
            free(resolved_call_ops);
            return 0;
        }
    }
    free(resolved_call_ops);

    s->block_seen[s->cur_block->id] = true;
    s->block_terminated[s->cur_block->id] = is_terminator(normalized.op);
    s->emitted_count++;
    return dest;
}

/* ---- IR-mode only ------------------------------------------------------ */

int lr_session_dump_ir(struct lr_session *s, FILE *out, session_error_t *err) {
    lr_func_t *f;
    err_clear(err);
    if (!s || !out) {
        err_set(err, S_ERR_ARGUMENT, "invalid dump arguments");
        return -1;
    }
    if (s->cfg.mode != SESSION_MODE_IR) {
        err_set(err, S_ERR_MODE, "IR dump requires IR mode");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, S_ERR_STATE, "cannot dump during active function");
        return -1;
    }
    for (f = s->module->first_func; f; f = f->next)
        lr_dump_func(f, s->module, out);
    return 0;
}

static int session_compile_parsed_module(struct lr_session *s, lr_module_t *m,
                                         const char *input_kind,
                                         void **out_addr,
                                         session_error_t *err) {
    lr_func_t *last_def = NULL;
    lr_owned_module_t *node = NULL;
    int rc;

    if (!s || !s->jit || !m) {
        err_set(err, S_ERR_ARGUMENT, "invalid compiled module arguments");
        return -1;
    }

    rc = lr_jit_add_module(s->jit, m);
    if (rc != 0) {
        lr_module_free(m);
        err_set(err, S_ERR_BACKEND, "%s module code generation failed",
                input_kind ? input_kind : "input");
        return -1;
    }

    node = (lr_owned_module_t *)calloc(1, sizeof(*node));
    if (!node) {
        lr_module_free(m);
        err_set(err, S_ERR_BACKEND, "module ownership registration failed");
        return -1;
    }
    node->module = m;
    node->next = s->owned_modules;
    s->owned_modules = node;

    if (!out_addr)
        return 0;

    *out_addr = NULL;
    for (last_def = m->first_func; last_def; last_def = last_def->next) {
        if (!last_def->is_decl && last_def->name && last_def->name[0])
            *out_addr = lr_jit_get_function(s->jit, last_def->name);
    }

    if (!*out_addr) {
        err_set(err, S_ERR_NOT_FOUND, "no defined function found in %s input",
                input_kind ? input_kind : "module");
        return -1;
    }
    return 0;
}

/* ---- Convenience: parse+compile .ll text ------------------------------- */

int lr_session_compile_ll(struct lr_session *s, const char *src, size_t len,
                           void **out_addr, session_error_t *err) {
    char parse_err[256];
    lr_module_t *m = NULL;

    err_clear(err);
    if (out_addr)
        *out_addr = NULL;
    if (!s || !s->jit || !src || len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid ll input");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, S_ERR_STATE, "cannot parse ll during active function");
        return -1;
    }

    parse_err[0] = '\0';
    m = lr_parse_ll(src, len, parse_err, sizeof(parse_err));
    if (!m) {
        err_set(err, S_ERR_PARSE, "ll parse failed: %s",
                parse_err[0] ? parse_err : "unknown error");
        return -1;
    }

    return session_compile_parsed_module(s, m, "ll", out_addr, err);
}

int lr_session_compile_bc(struct lr_session *s, const uint8_t *data, size_t len,
                          void **out_addr, session_error_t *err) {
    char parse_err[256];
    lr_arena_t *arena = NULL;
    lr_module_t *m = NULL;

    err_clear(err);
    if (out_addr)
        *out_addr = NULL;
    if (!s || !s->jit || !data || len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid bc input");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, S_ERR_STATE, "cannot parse bc during active function");
        return -1;
    }

    parse_err[0] = '\0';
    arena = lr_arena_create(0);
    if (!arena) {
        err_set(err, S_ERR_BACKEND, "arena allocation failed");
        return -1;
    }
    m = lr_parse_bc_streaming(data, len, arena, NULL, NULL, parse_err, sizeof(parse_err));
    if (!m) {
        lr_arena_destroy(arena);
        err_set(err, S_ERR_PARSE, "bc parse failed: %s",
                parse_err[0] ? parse_err : "unknown error");
        return -1;
    }

    return session_compile_parsed_module(s, m, "bc", out_addr, err);
}

int lr_session_compile_auto(struct lr_session *s, const uint8_t *data, size_t len,
                            void **out_addr, session_error_t *err) {
    char parse_err[256];
    lr_module_t *m = NULL;

    err_clear(err);
    if (out_addr)
        *out_addr = NULL;
    if (!s || !s->jit || !data || len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid auto input");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, S_ERR_STATE, "cannot parse input during active function");
        return -1;
    }

    parse_err[0] = '\0';
    m = lr_parse_auto(data, len, parse_err, sizeof(parse_err));
    if (!m) {
        err_set(err, S_ERR_PARSE, "auto parse failed: %s",
                parse_err[0] ? parse_err : "unknown error");
        return -1;
    }

    return session_compile_parsed_module(s, m, "auto", out_addr, err);
}

/* ---- Output ------------------------------------------------------------ */

int lr_session_emit_object(struct lr_session *s, const char *path,
                            session_error_t *err) {
    char backend_err[256] = {0};

    err_clear(err);
    if (!s || !s->module || !path) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_object arguments");
        return -1;
    }
    if (lr_emit_module_object_path(s->module, s->cfg.target, path,
                                   backend_err, sizeof(backend_err)) != 0) {
        err_set(err, S_ERR_BACKEND, "%s",
                backend_err[0] ? backend_err : "object emission failed");
        return -1;
    }
    return 0;
}

int lr_session_emit_exe(struct lr_session *s, const char *path,
                         session_error_t *err) {
    char backend_err[256] = {0};

    err_clear(err);
    if (!s || !s->module || !path) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_exe arguments");
        return -1;
    }
    if (lr_emit_module_executable_path(s->module, s->cfg.target, path,
                                       "_start", NULL, 0,
                                       backend_err, sizeof(backend_err)) != 0) {
        err_set(err, S_ERR_BACKEND, "%s",
                backend_err[0] ? backend_err : "executable emission failed");
        return -1;
    }
    return 0;
}

int lr_session_emit_exe_with_runtime(struct lr_session *s, const char *path,
                                      const char *runtime_ll, size_t runtime_len,
                                      session_error_t *err) {
    char backend_err[256] = {0};

    err_clear(err);
    if (!s || !s->module || !path || !runtime_ll || runtime_len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_exe_with_runtime arguments");
        return -1;
    }
    if (lr_emit_module_executable_path(s->module, s->cfg.target, path,
                                       "_start", runtime_ll, runtime_len,
                                       backend_err, sizeof(backend_err)) != 0) {
        err_set(err, S_ERR_BACKEND, "%s",
                backend_err[0] ? backend_err :
                "executable emission with runtime failed");
        return -1;
    }
    return 0;
}

/* ---- Access to underlying module --------------------------------------- */

lr_module_t *lr_session_module(struct lr_session *s) {
    return s ? s->module : NULL;
}
