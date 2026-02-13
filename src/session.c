#include "arena.h"
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

typedef struct session_stream_inst {
    session_inst_desc_t desc;
    lr_operand_desc_t *operands;
    uint32_t *indices;
    uint32_t block_id;
    struct session_stream_inst *next;
} session_stream_inst_t;

typedef struct session_stream_state {
    bool enabled;
    bool supported;
    uint32_t count;
    session_stream_inst_t *first;
    session_stream_inst_t *last;
} session_stream_state_t;

struct lr_session {
    session_config_t cfg;
    lr_jit_t *jit;
    lr_module_t *module;
    lr_func_t *cur_func;
    lr_block_t *cur_block;
    lr_block_t **blocks;
    uint32_t block_count;
    uint32_t block_cap;
    lr_owned_module_t *owned_modules;
    session_stream_state_t stream;
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

static void stream_reset(session_stream_state_t *stream) {
    session_stream_inst_t *it;
    if (!stream)
        return;
    it = stream->first;
    while (it) {
        session_stream_inst_t *next = it->next;
        free(it->operands);
        free(it->indices);
        free(it);
        it = next;
    }
    stream->enabled = false;
    stream->supported = false;
    stream->count = 0;
    stream->first = NULL;
    stream->last = NULL;
}

static void stream_begin(struct lr_session *s) {
    bool can_stream;
    if (!s)
        return;
    stream_reset(&s->stream);
    can_stream = (s->cfg.mode == SESSION_MODE_DIRECT &&
                  s->jit &&
                  s->jit->mode == LR_COMPILE_COPY_PATCH &&
                  s->jit->target &&
                  s->jit->target->compile_func_cp);
    s->stream.enabled = can_stream;
    s->stream.supported = can_stream;
}

static bool stream_operand_supported(const lr_operand_desc_t *op) {
    if (!op)
        return false;
    return op->kind == LR_OP_KIND_VREG || op->kind == LR_OP_KIND_IMM_I64;
}

static bool stream_inst_supported(const session_inst_desc_t *inst) {
    lr_type_kind_t k;
    if (!inst)
        return false;
    switch (inst->op) {
    case LR_OP_RET:
        if (inst->num_operands < 1 || !inst->operands || !inst->type)
            return false;
        k = inst->type->kind;
        if (k != LR_TYPE_I32 && k != LR_TYPE_I64)
            return false;
        return stream_operand_supported(&inst->operands[0]);
    case LR_OP_RET_VOID:
        return true;
    case LR_OP_ADD:
    case LR_OP_SUB:
    case LR_OP_AND:
    case LR_OP_OR:
    case LR_OP_XOR:
    case LR_OP_MUL:
    case LR_OP_SDIV:
    case LR_OP_SREM:
    case LR_OP_SHL:
    case LR_OP_LSHR:
    case LR_OP_ASHR:
        if (!inst->type || inst->num_operands < 2 || !inst->operands)
            return false;
        k = inst->type->kind;
        if (k != LR_TYPE_I32 && k != LR_TYPE_I64)
            return false;
        return stream_operand_supported(&inst->operands[0]) &&
               stream_operand_supported(&inst->operands[1]);
    default:
        return false;
    }
}

static int stream_append(struct lr_session *s, const session_inst_desc_t *inst,
                         uint32_t block_id, session_error_t *err) {
    session_stream_inst_t *node = NULL;
    if (!s || !inst)
        return -1;
    node = (session_stream_inst_t *)calloc(1, sizeof(*node));
    if (!node) {
        err_set(err, S_ERR_BACKEND, "stream instruction allocation failed");
        return -1;
    }
    node->desc = *inst;
    node->block_id = block_id;
    if (inst->num_operands > 0) {
        node->operands = (lr_operand_desc_t *)calloc(inst->num_operands, sizeof(*node->operands));
        if (!node->operands) {
            free(node);
            err_set(err, S_ERR_BACKEND, "stream operand allocation failed");
            return -1;
        }
        memcpy(node->operands, inst->operands, inst->num_operands * sizeof(*node->operands));
        node->desc.operands = node->operands;
    }
    if (inst->num_indices > 0) {
        node->indices = (uint32_t *)calloc(inst->num_indices, sizeof(*node->indices));
        if (!node->indices) {
            free(node->operands);
            free(node);
            err_set(err, S_ERR_BACKEND, "stream index allocation failed");
            return -1;
        }
        memcpy(node->indices, inst->indices, inst->num_indices * sizeof(*node->indices));
        node->desc.indices = node->indices;
    }
    if (!s->stream.first) {
        s->stream.first = node;
        s->stream.last = node;
    } else {
        s->stream.last->next = node;
        s->stream.last = node;
    }
    s->stream.count++;
    return 0;
}

static int validate_stream_blocks(struct lr_session *s, session_error_t *err) {
    bool *seen = NULL;
    bool *terminated = NULL;
    session_stream_inst_t *it;
    uint32_t i;
    if (!s || !s->cur_func)
        return -1;
    if (s->block_count == 0) {
        err_set(err, S_ERR_STATE, "block 0 is not terminated");
        return -1;
    }
    seen = (bool *)calloc(s->block_count, sizeof(*seen));
    terminated = (bool *)calloc(s->block_count, sizeof(*terminated));
    if (!seen || !terminated) {
        free(seen);
        free(terminated);
        err_set(err, S_ERR_BACKEND, "stream validation allocation failed");
        return -1;
    }
    for (it = s->stream.first; it; it = it->next) {
        if (it->block_id >= s->block_count) {
            free(seen);
            free(terminated);
            err_set(err, S_ERR_STATE, "invalid streamed block id");
            return -1;
        }
        seen[it->block_id] = true;
        terminated[it->block_id] = is_terminator(it->desc.op);
    }
    for (i = 0; i < s->block_count; i++) {
        if (!seen[i] || !terminated[i]) {
            free(seen);
            free(terminated);
            err_set(err, S_ERR_STATE, "block %u is not terminated", i);
            return -1;
        }
    }
    free(seen);
    free(terminated);
    return 0;
}

static int stream_can_compile_direct(struct lr_session *s) {
#if !defined(__x86_64__) && !defined(_M_X64)
    (void)s;
    return 0;
#else
    bool saw_terminator = false;
    session_stream_inst_t *it;

    if (!s || !s->stream.enabled || !s->stream.supported || !s->jit ||
        !s->jit->target || !s->jit->target->compile_func_cp || !s->cur_func)
        return 0;
    if (s->block_count != 1 || s->cur_func->num_params > 6 || s->cur_func->vararg)
        return 0;
    for (it = s->stream.first; it; it = it->next) {
        if (it->block_id != 0 || !stream_inst_supported(&it->desc))
            return 0;
        if (saw_terminator)
            return 0;
        if (is_terminator(it->desc.op))
            saw_terminator = true;
    }
    return saw_terminator ? 1 : 0;
#endif
}

static int stream_compile_direct(struct lr_session *s, void **out_addr,
                                 session_error_t *err) {
#if !defined(__x86_64__) && !defined(_M_X64)
    (void)s;
    (void)out_addr;
    err_set(err, S_ERR_BACKEND, "stream direct compile not supported on this target");
    return -1;
#else
    lr_jit_t *j = NULL;
    session_stream_inst_t *it = NULL;
    lr_inst_t *insts = NULL;
    lr_inst_t **inst_ptrs = NULL;
    lr_operand_t **inst_operands = NULL;
    lr_block_t block;
    lr_block_t *blocks[1];
    lr_func_t func;
    uint32_t i = 0;
    int rc = -1;
    size_t code_len = 0;
    bool opened_update = false;

    if (!s || !s->cur_func || !s->jit || !s->jit->target ||
        !s->jit->target->compile_func_cp || !s->cur_func->name) {
        err_set(err, S_ERR_STATE, "stream direct compile unavailable");
        return -1;
    }

    j = s->jit;
    insts = (lr_inst_t *)calloc(s->stream.count, sizeof(*insts));
    inst_ptrs = (lr_inst_t **)calloc(s->stream.count, sizeof(*inst_ptrs));
    inst_operands = (lr_operand_t **)calloc(s->stream.count, sizeof(*inst_operands));
    if (!insts || !inst_ptrs || !inst_operands) {
        err_set(err, S_ERR_BACKEND, "stream lowering allocation failed");
        goto cleanup;
    }

    for (it = s->stream.first; it; it = it->next, i++) {
        uint32_t oi;
        insts[i].op = it->desc.op;
        insts[i].type = it->desc.type;
        insts[i].dest = it->desc.dest;
        insts[i].num_operands = it->desc.num_operands;
        if (it->desc.num_operands > 0) {
            inst_operands[i] = (lr_operand_t *)calloc(it->desc.num_operands, sizeof(*inst_operands[i]));
            if (!inst_operands[i]) {
                err_set(err, S_ERR_BACKEND, "stream operand lowering allocation failed");
                goto cleanup;
            }
            for (oi = 0; oi < it->desc.num_operands; oi++)
                inst_operands[i][oi] = desc_to_op(&it->desc.operands[oi]);
            insts[i].operands = inst_operands[i];
        }
        inst_ptrs[i] = &insts[i];
    }

    memset(&block, 0, sizeof(block));
    block.id = 0;
    block.inst_array = inst_ptrs;
    block.num_insts = s->stream.count;

    memset(&func, 0, sizeof(func));
    blocks[0] = &block;
    func.name = s->cur_func->name;
    func.num_params = s->cur_func->num_params;
    func.param_vregs = s->cur_func->param_vregs;
    func.vararg = s->cur_func->vararg;
    func.next_vreg = s->cur_func->next_vreg;
    func.num_blocks = 1;
    func.block_array = blocks;

    if (!j->update_active) {
        lr_jit_begin_update(j);
        opened_update = j->update_active;
    }
    if (!j->update_active) {
        err_set(err, S_ERR_BACKEND, "jit update transition failed");
        goto cleanup;
    }

    {
        size_t free_space = j->code_cap - j->code_size;
        uint8_t *func_start = j->code_buf + j->code_size;
        rc = j->target->compile_func_cp(&func, s->module, func_start,
                                        free_space, &code_len, j->arena);
        if (rc != 0 || code_len > free_space) {
            err_set(err, S_ERR_BACKEND, "stream direct compile failed");
            rc = -1;
            goto cleanup;
        }
        j->code_size += code_len;
        if (j->update_active && code_len > 0)
            j->update_dirty = true;
        lr_jit_add_symbol(j, s->cur_func->name, func_start);
        s->cur_func->is_decl = true;
        if (out_addr)
            *out_addr = func_start;
    }

    rc = 0;
cleanup:
    if (opened_update && j->update_active)
        lr_jit_end_update(j);
    if (inst_operands) {
        for (i = 0; i < s->stream.count; i++)
            free(inst_operands[i]);
    }
    free(inst_operands);
    free(inst_ptrs);
    free(insts);
    return rc;
#endif
}

static int session_emit_ir_desc(struct lr_session *s, const session_inst_desc_t *inst,
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
            ops[i] = desc_to_op(&inst->operands[i]);
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

static int stream_replay_to_ir(struct lr_session *s, session_error_t *err) {
    session_stream_inst_t *it;
    if (!s)
        return -1;
    for (it = s->stream.first; it; it = it->next) {
        if (ensure_block(s, it->block_id, err) != 0)
            return -1;
        s->cur_block = s->blocks[it->block_id];
        if (session_emit_ir_desc(s, &it->desc, err, NULL) != 0)
            return -1;
    }
    return 0;
}

static int ensure_block_capacity(struct lr_session *s, uint32_t need) {
    lr_block_t **new_blocks = NULL;
    uint32_t new_cap = 0;
    if (need <= s->block_cap)
        return 0;
    new_cap = s->block_cap == 0 ? 8u : s->block_cap;
    while (new_cap < need)
        new_cap *= 2u;
    new_blocks = (lr_block_t **)calloc(new_cap, sizeof(*new_blocks));
    if (!new_blocks)
        return -1;
    if (s->block_cap > 0)
        memcpy(new_blocks, s->blocks, sizeof(*new_blocks) * s->block_cap);
    free(s->blocks);
    s->blocks = new_blocks;
    s->block_cap = new_cap;
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

static int validate_function_blocks(struct lr_session *s,
                                     session_error_t *err) {
    uint32_t i;
    if (!s || !s->cur_func)
        return -1;
    for (i = 0; i < s->block_count; i++) {
        lr_block_t *b = s->blocks[i];
        if (!b || !b->last || !is_terminator(b->last->op)) {
            err_set(err, S_ERR_STATE, "block %u is not terminated", i);
            return -1;
        }
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

    stream_reset(&s->stream);
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
    stream_reset(&s->stream);
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
    stream_begin(s);
    return 0;
}

uint32_t lr_session_param(struct lr_session *s, uint32_t idx) {
    if (!s || !s->cur_func || idx >= s->cur_func->num_params)
        return UINT32_MAX;
    return s->cur_func->param_vregs[idx];
}

int lr_session_func_end(struct lr_session *s, void **out_addr,
                         session_error_t *err) {
    int rc;
    err_clear(err);
    if (!s || !s->cur_func) {
        err_set(err, S_ERR_STATE, "no active function");
        return -1;
    }
    if (s->stream.enabled) {
        if (validate_stream_blocks(s, err) != 0)
            return -1;

        if (stream_can_compile_direct(s)) {
            if (stream_compile_direct(s, out_addr, err) == 0) {
                stream_reset(&s->stream);
                s->cur_func = NULL;
                s->cur_block = NULL;
                s->block_count = 0;
                return 0;
            }
            err_clear(err);
        }

        if (stream_replay_to_ir(s, err) != 0)
            return -1;
    }

    if (validate_function_blocks(s, err) != 0)
        return -1;

    rc = compile_current_function(s, out_addr, err);
    if (rc != 0)
        return -1;

    stream_reset(&s->stream);
    s->cur_func = NULL;
    s->cur_block = NULL;
    s->block_count = 0;
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

    if (s->stream.enabled) {
        if (s->block_count != 1 || s->cur_block->id != 0 ||
            !stream_inst_supported(&normalized))
            s->stream.supported = false;
        if (stream_append(s, &normalized, s->cur_block->id, err) != 0)
            return 0;
        return dest;
    }

    if (session_emit_ir_desc(s, &normalized, err, NULL) != 0)
        return 0;
    return dest;
}

/* ---- IR-mode only ------------------------------------------------------ */

int lr_session_dump_ir(struct lr_session *s, FILE *out, session_error_t *err) {
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
    lr_module_dump(s->module, out);
    return 0;
}

/* ---- Convenience: parse+compile .ll text ------------------------------- */

int lr_session_compile_ll(struct lr_session *s, const char *src, size_t len,
                           void **out_addr, session_error_t *err) {
    char parse_err[256];
    lr_module_t *m = NULL;
    lr_func_t *last_def = NULL;
    lr_owned_module_t *node = NULL;
    int rc;

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

    rc = lr_jit_add_module(s->jit, m);
    if (rc != 0) {
        lr_module_free(m);
        err_set(err, S_ERR_BACKEND, "ll module code generation failed");
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

    for (last_def = m->first_func; last_def; last_def = last_def->next) {
        if (!last_def->is_decl && last_def->name && last_def->name[0]) {
            if (out_addr)
                *out_addr = lr_jit_get_function(s->jit, last_def->name);
        }
    }

    if (out_addr && !*out_addr) {
        err_set(err, S_ERR_NOT_FOUND, "no defined function found in ll input");
        return -1;
    }
    return 0;
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
