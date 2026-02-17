#include "wasm_to_ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WASM value types */
#define VT_I32 0x7F
#define VT_I64 0x7E
#define VT_F32 0x7D
#define VT_F64 0x7C

/* WASM opcodes */
#define OP_UNREACHABLE  0x00
#define OP_NOP          0x01
#define OP_BLOCK        0x02
#define OP_LOOP         0x03
#define OP_IF           0x04
#define OP_ELSE         0x05
#define OP_END          0x0B
#define OP_BR           0x0C
#define OP_BR_IF        0x0D
#define OP_RETURN       0x0F
#define OP_CALL         0x10
#define OP_DROP         0x1A
#define OP_SELECT       0x1B
#define OP_LOCAL_GET    0x20
#define OP_LOCAL_SET    0x21
#define OP_LOCAL_TEE    0x22
#define OP_GLOBAL_GET   0x23
#define OP_GLOBAL_SET   0x24
#define OP_I32_LOAD     0x28
#define OP_I64_LOAD     0x29
#define OP_I32_LOAD8_S  0x2C
#define OP_I32_LOAD8_U  0x2D
#define OP_I32_LOAD16_S 0x2E
#define OP_I32_LOAD16_U 0x2F
#define OP_I32_STORE    0x36
#define OP_I64_STORE    0x37
#define OP_I32_STORE8   0x3A
#define OP_I32_STORE16  0x3B
#define OP_I32_CONST    0x41
#define OP_I64_CONST    0x42
#define OP_I32_EQZ      0x45
#define OP_I32_EQ       0x46
#define OP_I32_NE       0x47
#define OP_I32_LT_S     0x48
#define OP_I32_LT_U     0x49
#define OP_I32_GT_S     0x4A
#define OP_I32_GT_U     0x4B
#define OP_I32_LE_S     0x4C
#define OP_I32_LE_U     0x4D
#define OP_I32_GE_S     0x4E
#define OP_I32_GE_U     0x4F
#define OP_I64_EQZ      0x50
#define OP_I64_EQ       0x51
#define OP_I64_NE       0x52
#define OP_I64_LT_S     0x53
#define OP_I64_LT_U     0x54
#define OP_I64_GT_S     0x55
#define OP_I64_GT_U     0x56
#define OP_I64_LE_S     0x57
#define OP_I64_LE_U     0x58
#define OP_I64_GE_S     0x59
#define OP_I64_GE_U     0x5A
#define OP_I32_CLZ      0x67
#define OP_I32_CTZ      0x68
#define OP_I32_ADD      0x6A
#define OP_I32_SUB      0x6B
#define OP_I32_MUL      0x6C
#define OP_I32_DIV_S    0x6D
#define OP_I32_DIV_U    0x6E
#define OP_I32_REM_S    0x6F
#define OP_I32_REM_U    0x70
#define OP_I32_AND      0x71
#define OP_I32_OR       0x72
#define OP_I32_XOR      0x73
#define OP_I32_SHL      0x74
#define OP_I32_SHR_S    0x75
#define OP_I32_SHR_U    0x76
#define OP_I64_ADD      0x7C
#define OP_I64_SUB      0x7D
#define OP_I64_MUL      0x7E
#define OP_I64_DIV_S    0x7F
#define OP_I64_DIV_U    0x80
#define OP_I64_REM_S    0x81
#define OP_I64_REM_U    0x82
#define OP_I64_AND      0x83
#define OP_I64_OR       0x84
#define OP_I64_XOR      0x85
#define OP_I64_SHL      0x86
#define OP_I64_SHR_S    0x87
#define OP_I64_SHR_U    0x88
#define OP_I32_WRAP_I64     0xA7
#define OP_I64_EXTEND_I32_S 0xAC
#define OP_I64_EXTEND_I32_U 0xAD

/* Block type for 0x40 (empty) */
#define BLOCKTYPE_VOID 0x40

/* Control stack entry kinds */
#define CS_BLOCK 0
#define CS_LOOP  1
#define CS_IF    2

#define MAX_STACK 256
#define MAX_CTRL  64

typedef int (*lr_wasm_inst_callback_t)(lr_func_t *func, lr_block_t *block,
                                       const lr_inst_t *inst, void *ctx);

typedef struct {
    uint32_t vregs[MAX_STACK];
    lr_type_t *types[MAX_STACK];
    uint32_t top;
} val_stack_t;

typedef struct {
    uint8_t kind;           /* CS_BLOCK, CS_LOOP, CS_IF */
    lr_block_t *cont_block; /* continuation (after end) */
    lr_block_t *loop_hdr;   /* loop header (for CS_LOOP) */
    lr_block_t *else_block; /* for CS_IF: else target */
    lr_type_t *result_type; /* NULL for void blocks */
    uint32_t result_slot;   /* alloca vreg for block result (if result_type != NULL) */
    uint32_t stack_height;  /* value stack height at block entry */
} ctrl_entry_t;

typedef struct {
    lr_module_t *mod;
    lr_arena_t *arena;
    lr_func_t *func;
    lr_block_t *cur_block;
    val_stack_t vstack;
    ctrl_entry_t ctrl[MAX_CTRL];
    uint32_t ctrl_top;
    uint32_t *local_slots; /* vreg IDs of alloca slots for locals */
    uint32_t num_locals;
    char *err;
    size_t errlen;
    bool failed;
    lr_wasm_inst_callback_t on_inst;
    void *on_inst_ctx;
} wasm_ctx_t;

static void ctx_err(wasm_ctx_t *ctx, const char *msg) {
    if (ctx->err && ctx->errlen > 0)
        snprintf(ctx->err, ctx->errlen, "%s", msg);
    ctx->failed = true;
}

static void vpush(wasm_ctx_t *ctx, uint32_t vreg, lr_type_t *type) {
    if (ctx->vstack.top >= MAX_STACK) { ctx_err(ctx, "value stack overflow"); return; }
    ctx->vstack.vregs[ctx->vstack.top] = vreg;
    ctx->vstack.types[ctx->vstack.top] = type;
    ctx->vstack.top++;
}

static uint32_t vpop(wasm_ctx_t *ctx, lr_type_t **type_out) {
    if (ctx->vstack.top == 0) { ctx_err(ctx, "value stack underflow"); return 0; }
    ctx->vstack.top--;
    if (type_out) *type_out = ctx->vstack.types[ctx->vstack.top];
    return ctx->vstack.vregs[ctx->vstack.top];
}

static lr_type_t *wasm_to_lr_type(lr_module_t *m, uint8_t vt) {
    switch (vt) {
    case VT_I32: return m->type_i32;
    case VT_I64: return m->type_i64;
    case VT_F32: return m->type_float;
    case VT_F64: return m->type_double;
    default:     return m->type_i32;
    }
}

static void append_inst(wasm_ctx_t *ctx, lr_inst_t *inst) {
    if (!ctx || !inst)
        return;
    lr_block_append(ctx->cur_block, inst);
    if (ctx->on_inst &&
        ctx->on_inst(ctx->func, ctx->cur_block, inst, ctx->on_inst_ctx) != 0) {
        ctx_err(ctx, "wasm streaming callback failed");
    }
}

static void emit_inst(wasm_ctx_t *ctx, lr_opcode_t op, lr_type_t *type,
                      uint32_t dest, lr_operand_t *ops, uint32_t nops) {
    lr_inst_t *inst;
    if (!ctx || ctx->failed)
        return;
    inst = lr_inst_create(ctx->arena, op, type, dest, ops, nops);
    if (!inst) {
        ctx_err(ctx, "failed to allocate WASM IR instruction");
        return;
    }
    append_inst(ctx, inst);
}

static void emit_icmp(wasm_ctx_t *ctx, lr_type_t *type, uint32_t dest,
                      lr_operand_t *ops, uint32_t nops, lr_icmp_pred_t pred) {
    lr_inst_t *inst;
    if (!ctx || ctx->failed)
        return;
    inst = lr_inst_create(ctx->arena, LR_OP_ICMP, type, dest, ops, nops);
    if (!inst) {
        ctx_err(ctx, "failed to allocate WASM IR instruction");
        return;
    }
    inst->icmp_pred = pred;
    append_inst(ctx, inst);
}

/* Read a LEB128 u32 from the body bytes, advancing *pos */
static uint32_t body_u32(const uint8_t *body, size_t body_len, size_t *pos) {
    uint32_t val;
    size_t n = lr_wasm_read_leb_u32(body + *pos, body_len - *pos, &val);
    *pos += n;
    return val;
}

static int32_t body_i32(const uint8_t *body, size_t body_len, size_t *pos) {
    int32_t val;
    size_t n = lr_wasm_read_leb_i32(body + *pos, body_len - *pos, &val);
    *pos += n;
    return val;
}

static int64_t body_i64(const uint8_t *body, size_t body_len, size_t *pos) {
    int64_t val;
    size_t n = lr_wasm_read_leb_i64(body + *pos, body_len - *pos, &val);
    *pos += n;
    return val;
}

/* Emit a binary op: pop 2, compute, push result */
static void emit_binop(wasm_ctx_t *ctx, lr_opcode_t op) {
    lr_type_t *t = NULL;
    uint32_t rhs = vpop(ctx, NULL);
    uint32_t lhs = vpop(ctx, &t);
    uint32_t dest = lr_vreg_new(ctx->func);
    lr_operand_t ops[2] = { lr_op_vreg(lhs, t), lr_op_vreg(rhs, t) };
    emit_inst(ctx, op, t, dest, ops, 2);
    vpush(ctx, dest, t);
}

/* Emit a comparison: pop 2, icmp, push i1 result */
static void emit_cmp(wasm_ctx_t *ctx, lr_icmp_pred_t pred) {
    lr_type_t *t = NULL;
    uint32_t rhs = vpop(ctx, NULL);
    uint32_t lhs = vpop(ctx, &t);
    uint32_t dest = lr_vreg_new(ctx->func);
    lr_operand_t ops[2] = { lr_op_vreg(lhs, t), lr_op_vreg(rhs, t) };
    emit_icmp(ctx, ctx->mod->type_i1, dest, ops, 2, pred);
    vpush(ctx, dest, ctx->mod->type_i32);
}

/* Parse block type: 0x40=void, or a value type byte */
static lr_type_t *parse_blocktype(wasm_ctx_t *ctx, const uint8_t *body,
                                   size_t body_len, size_t *pos) {
    if (*pos >= body_len) return NULL;
    uint8_t bt = body[*pos];
    (*pos)++;
    if (bt == BLOCKTYPE_VOID) return NULL;
    return wasm_to_lr_type(ctx->mod, bt);
}

static void ctrl_push(wasm_ctx_t *ctx, uint8_t kind, lr_block_t *cont,
                      lr_block_t *loop_hdr, lr_block_t *else_blk,
                      lr_type_t *result_type, uint32_t result_slot) {
    if (ctx->ctrl_top >= MAX_CTRL) { ctx_err(ctx, "control stack overflow"); return; }
    ctx->ctrl[ctx->ctrl_top].kind = kind;
    ctx->ctrl[ctx->ctrl_top].cont_block = cont;
    ctx->ctrl[ctx->ctrl_top].loop_hdr = loop_hdr;
    ctx->ctrl[ctx->ctrl_top].else_block = else_blk;
    ctx->ctrl[ctx->ctrl_top].result_type = result_type;
    ctx->ctrl[ctx->ctrl_top].result_slot = result_slot;
    ctx->ctrl[ctx->ctrl_top].stack_height = ctx->vstack.top;
    ctx->ctrl_top++;
}

static void convert_func_body(wasm_ctx_t *ctx, const lr_wasm_module_t *wmod,
                               uint32_t func_idx) {
    const lr_wasm_code_t *code = &wmod->codes[func_idx];
    const uint8_t *body = code->body;
    size_t body_len = code->body_len;
    size_t pos = 0;
    lr_module_t *m = ctx->mod;

    /* Allocate locals (params + declared locals) as alloca slots */
    uint32_t num_params = ctx->func->num_params;
    uint32_t num_declared = 0;
    for (uint32_t i = 0; i < code->num_local_groups; i++)
        num_declared += code->local_groups[i].count;
    ctx->num_locals = num_params + num_declared;
    ctx->local_slots = lr_arena_array(ctx->arena, uint32_t, ctx->num_locals);

    /* Entry block: emit alloca for each local, store params */
    for (uint32_t i = 0; i < num_params; i++) {
        uint32_t slot = lr_vreg_new(ctx->func);
        lr_operand_t ops[1] = { lr_op_imm_i64(1, m->type_i32) };
        emit_inst(ctx, LR_OP_ALLOCA, ctx->func->param_types[i], slot, ops, 1);
        ctx->local_slots[i] = slot;
        /* Store param into slot */
        lr_operand_t store_ops[2] = {
            lr_op_vreg(ctx->func->param_vregs[i], ctx->func->param_types[i]),
            lr_op_vreg(slot, m->type_ptr)
        };
        uint32_t dummy = lr_vreg_new(ctx->func);
        emit_inst(ctx, LR_OP_STORE, m->type_void, dummy, store_ops, 2);
    }

    /* Alloca for declared locals */
    uint32_t local_idx = num_params;
    for (uint32_t i = 0; i < code->num_local_groups; i++) {
        lr_type_t *lt = wasm_to_lr_type(m, code->local_groups[i].type);
        for (uint32_t j = 0; j < code->local_groups[i].count; j++) {
            uint32_t slot = lr_vreg_new(ctx->func);
            lr_operand_t ops[1] = { lr_op_imm_i64(1, m->type_i32) };
            emit_inst(ctx, LR_OP_ALLOCA, lt, slot, ops, 1);
            ctx->local_slots[local_idx++] = slot;
            /* Initialize to zero */
            lr_operand_t store_ops[2] = {
                lr_op_imm_i64(0, lt),
                lr_op_vreg(slot, m->type_ptr)
            };
            uint32_t dummy = lr_vreg_new(ctx->func);
            emit_inst(ctx, LR_OP_STORE, m->type_void, dummy, store_ops, 2);
        }
    }

    /* Push the implicit function-level block */
    lr_block_t *func_exit = lr_block_create(ctx->func, ctx->arena, "func_exit");
    ctrl_push(ctx, CS_BLOCK, func_exit, NULL, NULL, ctx->func->ret_type, 0);

    while (pos < body_len && !ctx->failed) {
        uint8_t op = body[pos++];

        switch (op) {
        case OP_UNREACHABLE: {
            lr_operand_t dummy;
            memset(&dummy, 0, sizeof(dummy));
            emit_inst(ctx, LR_OP_UNREACHABLE, m->type_void, 0, &dummy, 0);
            break;
        }
        case OP_NOP:
            break;

        case OP_BLOCK: {
            lr_type_t *bt = parse_blocktype(ctx, body, body_len, &pos);
            lr_block_t *cont = lr_block_create(ctx->func, ctx->arena, "block_cont");
            uint32_t rslot = 0;
            if (bt) {
                rslot = lr_vreg_new(ctx->func);
                lr_operand_t aops[1] = { lr_op_imm_i64(1, m->type_i32) };
                emit_inst(ctx, LR_OP_ALLOCA, bt, rslot, aops, 1);
            }
            ctrl_push(ctx, CS_BLOCK, cont, NULL, NULL, bt, rslot);
            break;
        }
        case OP_LOOP: {
            lr_type_t *bt = parse_blocktype(ctx, body, body_len, &pos);
            lr_block_t *hdr = lr_block_create(ctx->func, ctx->arena, "loop_hdr");
            lr_block_t *cont = lr_block_create(ctx->func, ctx->arena, "loop_cont");
            lr_operand_t br_ops[1] = { lr_op_block(hdr->id) };
            emit_inst(ctx, LR_OP_BR, m->type_void, 0, br_ops, 1);
            ctx->cur_block = hdr;
            ctrl_push(ctx, CS_LOOP, cont, hdr, NULL, bt, 0);
            break;
        }
        case OP_IF: {
            lr_type_t *bt = parse_blocktype(ctx, body, body_len, &pos);
            uint32_t cond = vpop(ctx, NULL);
            lr_block_t *then_blk = lr_block_create(ctx->func, ctx->arena, "if_then");
            lr_block_t *else_blk = lr_block_create(ctx->func, ctx->arena, "if_else");
            lr_block_t *merge = lr_block_create(ctx->func, ctx->arena, "if_merge");
            uint32_t rslot = 0;
            if (bt) {
                rslot = lr_vreg_new(ctx->func);
                lr_operand_t aops[1] = { lr_op_imm_i64(1, m->type_i32) };
                emit_inst(ctx, LR_OP_ALLOCA, bt, rslot, aops, 1);
            }
            uint32_t cond_i1 = lr_vreg_new(ctx->func);
            lr_operand_t icmp_ops[2] = {
                lr_op_vreg(cond, m->type_i32),
                lr_op_imm_i64(0, m->type_i32)
            };
            emit_icmp(ctx, m->type_i1, cond_i1, icmp_ops, 2, LR_ICMP_NE);
            lr_operand_t br_ops[3] = {
                lr_op_vreg(cond_i1, m->type_i1),
                lr_op_block(then_blk->id),
                lr_op_block(else_blk->id)
            };
            emit_inst(ctx, LR_OP_CONDBR, m->type_void, 0, br_ops, 3);
            ctx->cur_block = then_blk;
            ctrl_push(ctx, CS_IF, merge, NULL, else_blk, bt, rslot);
            break;
        }
        case OP_ELSE: {
            if (ctx->ctrl_top == 0) { ctx_err(ctx, "else without if"); return; }
            ctrl_entry_t *ce = &ctx->ctrl[ctx->ctrl_top - 1];
            /* Store block result to result slot before branching */
            if (ce->result_type && ctx->vstack.top > ce->stack_height) {
                lr_type_t *t = NULL;
                uint32_t val = vpop(ctx, &t);
                lr_operand_t sops[2] = {
                    lr_op_vreg(val, t),
                    lr_op_vreg(ce->result_slot, m->type_ptr)
                };
                uint32_t d = lr_vreg_new(ctx->func);
                emit_inst(ctx, LR_OP_STORE, m->type_void, d, sops, 2);
            }
            lr_operand_t br_ops[1] = { lr_op_block(ce->cont_block->id) };
            emit_inst(ctx, LR_OP_BR, m->type_void, 0, br_ops, 1);
            ctx->cur_block = ce->else_block;
            ce->else_block = NULL;
            break;
        }
        case OP_END: {
            if (ctx->ctrl_top == 0) break;
            /* Check if this is the function-level block end */
            if (ctx->ctrl_top == 1) {
                ctx->ctrl_top--;
                ctrl_entry_t *ce = &ctx->ctrl[0];
                /* Emit return (the post-loop code will handle it) */
                lr_operand_t br_ops[1] = { lr_op_block(ce->cont_block->id) };
                emit_inst(ctx, LR_OP_BR, m->type_void, 0, br_ops, 1);
                ctx->cur_block = ce->cont_block;
                break;
            }
            ctx->ctrl_top--;
            ctrl_entry_t *ce = &ctx->ctrl[ctx->ctrl_top];
            /* Store block result before branching */
            if (ce->result_type && ce->result_slot &&
                ctx->vstack.top > ce->stack_height) {
                lr_type_t *t = NULL;
                uint32_t val = vpop(ctx, &t);
                lr_operand_t sops[2] = {
                    lr_op_vreg(val, t),
                    lr_op_vreg(ce->result_slot, m->type_ptr)
                };
                uint32_t d = lr_vreg_new(ctx->func);
                emit_inst(ctx, LR_OP_STORE, m->type_void, d, sops, 2);
            }
            if (ce->kind == CS_IF && ce->else_block) {
                lr_block_t *saved = ctx->cur_block;
                ctx->cur_block = ce->else_block;
                lr_operand_t br_ops[1] = { lr_op_block(ce->cont_block->id) };
                emit_inst(ctx, LR_OP_BR, m->type_void, 0, br_ops, 1);
                ctx->cur_block = saved;
            }
            lr_operand_t br_ops[1] = { lr_op_block(ce->cont_block->id) };
            emit_inst(ctx, LR_OP_BR, m->type_void, 0, br_ops, 1);
            ctx->cur_block = ce->cont_block;
            /* Load block result in continuation */
            if (ce->result_type && ce->result_slot) {
                uint32_t dest = lr_vreg_new(ctx->func);
                lr_operand_t lops[1] = { lr_op_vreg(ce->result_slot, m->type_ptr) };
                emit_inst(ctx, LR_OP_LOAD, ce->result_type, dest, lops, 1);
                vpush(ctx, dest, ce->result_type);
            }
            break;
        }
        case OP_BR: {
            uint32_t depth = body_u32(body, body_len, &pos);
            if (depth >= ctx->ctrl_top) { ctx_err(ctx, "br depth out of range"); return; }
            ctrl_entry_t *ce = &ctx->ctrl[ctx->ctrl_top - 1 - depth];
            lr_block_t *target = (ce->kind == CS_LOOP) ? ce->loop_hdr : ce->cont_block;
            lr_operand_t br_ops[1] = { lr_op_block(target->id) };
            emit_inst(ctx, LR_OP_BR, m->type_void, 0, br_ops, 1);
            /* Create a new unreachable block for subsequent dead code */
            ctx->cur_block = lr_block_create(ctx->func, ctx->arena, "dead");
            break;
        }
        case OP_BR_IF: {
            uint32_t depth = body_u32(body, body_len, &pos);
            if (depth >= ctx->ctrl_top) { ctx_err(ctx, "br_if depth out of range"); return; }
            uint32_t cond = vpop(ctx, NULL);
            ctrl_entry_t *ce = &ctx->ctrl[ctx->ctrl_top - 1 - depth];
            lr_block_t *target = (ce->kind == CS_LOOP) ? ce->loop_hdr : ce->cont_block;
            lr_block_t *fallthrough = lr_block_create(ctx->func, ctx->arena, "br_if_ft");
            uint32_t cond_i1 = lr_vreg_new(ctx->func);
            lr_operand_t icmp_ops[2] = {
                lr_op_vreg(cond, m->type_i32),
                lr_op_imm_i64(0, m->type_i32)
            };
            emit_icmp(ctx, m->type_i1, cond_i1, icmp_ops, 2, LR_ICMP_NE);
            lr_operand_t br_ops[3] = {
                lr_op_vreg(cond_i1, m->type_i1),
                lr_op_block(target->id),
                lr_op_block(fallthrough->id)
            };
            emit_inst(ctx, LR_OP_CONDBR, m->type_void, 0, br_ops, 3);
            ctx->cur_block = fallthrough;
            break;
        }
        case OP_RETURN: {
            if (ctx->func->ret_type->kind == LR_TYPE_VOID) {
                lr_operand_t dummy;
                memset(&dummy, 0, sizeof(dummy));
                emit_inst(ctx, LR_OP_RET_VOID, m->type_void, 0, &dummy, 0);
            } else {
                lr_type_t *t = NULL;
                uint32_t val = vpop(ctx, &t);
                lr_operand_t ops[1] = { lr_op_vreg(val, t) };
                emit_inst(ctx, LR_OP_RET, m->type_void, 0, ops, 1);
            }
            ctx->cur_block = lr_block_create(ctx->func, ctx->arena, "dead");
            break;
        }
        case OP_CALL: {
            uint32_t callee_idx = body_u32(body, body_len, &pos);
            /* Determine the function type */
            uint32_t type_idx;
            if (callee_idx < wmod->num_func_imports) {
                type_idx = wmod->imports[callee_idx].type_idx;
            } else {
                uint32_t local_idx = callee_idx - wmod->num_func_imports;
                if (local_idx >= wmod->num_funcs) { ctx_err(ctx, "call: bad func idx"); return; }
                type_idx = wmod->func_type_indices[local_idx];
            }
            if (type_idx >= wmod->num_types) { ctx_err(ctx, "call: bad type idx"); return; }
            const lr_wasm_functype_t *ft = &wmod->types[type_idx];

            /* Pop arguments (in reverse order from stack, but call expects forward) */
            uint32_t nargs = ft->num_params;
            uint32_t *arg_vregs = lr_arena_array(ctx->arena, uint32_t, nargs);
            lr_type_t **arg_types = lr_arena_array(ctx->arena, lr_type_t *, nargs);
            for (uint32_t i = nargs; i > 0; i--) {
                arg_vregs[i - 1] = vpop(ctx, &arg_types[i - 1]);
            }

            /* Find the callee function in the module */
            lr_func_t *callee = m->first_func;
            uint32_t ci = 0;
            while (callee && ci < callee_idx) { callee = callee->next; ci++; }

            lr_type_t *ret_type = (ft->num_results > 0) ?
                wasm_to_lr_type(m, ft->results[0]) : m->type_void;

            /* Build operands: [callee_global_id, arg0, arg1, ...] */
            uint32_t nops = 1 + nargs;
            lr_operand_t *ops = lr_arena_array(ctx->arena, lr_operand_t, nops);
            ops[0] = lr_op_global(callee_idx, m->type_ptr);
            for (uint32_t i = 0; i < nargs; i++)
                ops[1 + i] = lr_op_vreg(arg_vregs[i], arg_types[i]);

            uint32_t dest = (ft->num_results > 0) ? lr_vreg_new(ctx->func) : 0;
            emit_inst(ctx, LR_OP_CALL, ret_type, dest, ops, nops);

            if (ft->num_results > 0)
                vpush(ctx, dest, ret_type);
            break;
        }
        case OP_DROP:
            vpop(ctx, NULL);
            break;

        case OP_SELECT: {
            uint32_t cond = vpop(ctx, NULL);
            lr_type_t *t = NULL;
            uint32_t val2 = vpop(ctx, NULL);
            uint32_t val1 = vpop(ctx, &t);
            uint32_t cond_i1 = lr_vreg_new(ctx->func);
            lr_operand_t icmp_ops[2] = {
                lr_op_vreg(cond, m->type_i32),
                lr_op_imm_i64(0, m->type_i32)
            };
            emit_icmp(ctx, m->type_i1, cond_i1, icmp_ops, 2, LR_ICMP_NE);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t sel_ops[3] = {
                lr_op_vreg(cond_i1, m->type_i1),
                lr_op_vreg(val1, t),
                lr_op_vreg(val2, t)
            };
            emit_inst(ctx, LR_OP_SELECT, t, dest, sel_ops, 3);
            vpush(ctx, dest, t);
            break;
        }

        case OP_LOCAL_GET: {
            uint32_t idx = body_u32(body, body_len, &pos);
            if (idx >= ctx->num_locals) { ctx_err(ctx, "local.get: bad idx"); return; }
            /* Determine type from param_types or local groups */
            lr_type_t *lt;
            if (idx < ctx->func->num_params) {
                lt = ctx->func->param_types[idx];
            } else {
                uint32_t li = idx - ctx->func->num_params;
                uint32_t accum = 0;
                lt = m->type_i32;
                const lr_wasm_code_t *code_entry = &wmod->codes[func_idx];
                for (uint32_t g = 0; g < code_entry->num_local_groups; g++) {
                    if (li < accum + code_entry->local_groups[g].count) {
                        lt = wasm_to_lr_type(m, code_entry->local_groups[g].type);
                        break;
                    }
                    accum += code_entry->local_groups[g].count;
                }
            }
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[1] = { lr_op_vreg(ctx->local_slots[idx], m->type_ptr) };
            emit_inst(ctx, LR_OP_LOAD, lt, dest, ops, 1);
            vpush(ctx, dest, lt);
            break;
        }
        case OP_LOCAL_SET: {
            uint32_t idx = body_u32(body, body_len, &pos);
            if (idx >= ctx->num_locals) { ctx_err(ctx, "local.set: bad idx"); return; }
            lr_type_t *t = NULL;
            uint32_t val = vpop(ctx, &t);
            lr_operand_t ops[2] = {
                lr_op_vreg(val, t),
                lr_op_vreg(ctx->local_slots[idx], m->type_ptr)
            };
            uint32_t dummy = lr_vreg_new(ctx->func);
            emit_inst(ctx, LR_OP_STORE, m->type_void, dummy, ops, 2);
            break;
        }
        case OP_LOCAL_TEE: {
            uint32_t idx = body_u32(body, body_len, &pos);
            if (idx >= ctx->num_locals) { ctx_err(ctx, "local.tee: bad idx"); return; }
            lr_type_t *t = NULL;
            uint32_t val = vpop(ctx, &t);
            lr_operand_t ops[2] = {
                lr_op_vreg(val, t),
                lr_op_vreg(ctx->local_slots[idx], m->type_ptr)
            };
            uint32_t dummy = lr_vreg_new(ctx->func);
            emit_inst(ctx, LR_OP_STORE, m->type_void, dummy, ops, 2);
            vpush(ctx, val, t); /* push the value back */
            break;
        }

        case OP_I32_CONST: {
            int32_t val = body_i32(body, body_len, &pos);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[2] = {
                lr_op_imm_i64(0, m->type_i32),
                lr_op_imm_i64(val, m->type_i32)
            };
            emit_inst(ctx, LR_OP_ADD, m->type_i32, dest, ops, 2);
            vpush(ctx, dest, m->type_i32);
            break;
        }
        case OP_I64_CONST: {
            int64_t val = body_i64(body, body_len, &pos);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[2] = {
                lr_op_imm_i64(0, m->type_i64),
                lr_op_imm_i64(val, m->type_i64)
            };
            emit_inst(ctx, LR_OP_ADD, m->type_i64, dest, ops, 2);
            vpush(ctx, dest, m->type_i64);
            break;
        }

        case OP_I32_ADD: case OP_I64_ADD: emit_binop(ctx, LR_OP_ADD); break;
        case OP_I32_SUB: case OP_I64_SUB: emit_binop(ctx, LR_OP_SUB); break;
        case OP_I32_MUL: case OP_I64_MUL: emit_binop(ctx, LR_OP_MUL); break;
        case OP_I32_DIV_S: case OP_I64_DIV_S:
        case OP_I32_DIV_U: case OP_I64_DIV_U:
            emit_binop(ctx, LR_OP_SDIV);
            break;
        case OP_I32_REM_S: case OP_I64_REM_S:
        case OP_I32_REM_U: case OP_I64_REM_U:
            emit_binop(ctx, LR_OP_SREM);
            break;
        case OP_I32_AND: case OP_I64_AND: emit_binop(ctx, LR_OP_AND); break;
        case OP_I32_OR:  case OP_I64_OR:  emit_binop(ctx, LR_OP_OR); break;
        case OP_I32_XOR: case OP_I64_XOR: emit_binop(ctx, LR_OP_XOR); break;
        case OP_I32_SHL: case OP_I64_SHL: emit_binop(ctx, LR_OP_SHL); break;
        case OP_I32_SHR_S: case OP_I64_SHR_S: emit_binop(ctx, LR_OP_ASHR); break;
        case OP_I32_SHR_U: case OP_I64_SHR_U: emit_binop(ctx, LR_OP_LSHR); break;

        case OP_I32_EQ:  case OP_I64_EQ:  emit_cmp(ctx, LR_ICMP_EQ); break;
        case OP_I32_NE:  case OP_I64_NE:  emit_cmp(ctx, LR_ICMP_NE); break;
        case OP_I32_LT_S: case OP_I64_LT_S: emit_cmp(ctx, LR_ICMP_SLT); break;
        case OP_I32_LT_U: case OP_I64_LT_U: emit_cmp(ctx, LR_ICMP_ULT); break;
        case OP_I32_GT_S: case OP_I64_GT_S: emit_cmp(ctx, LR_ICMP_SGT); break;
        case OP_I32_GT_U: case OP_I64_GT_U: emit_cmp(ctx, LR_ICMP_UGT); break;
        case OP_I32_LE_S: case OP_I64_LE_S: emit_cmp(ctx, LR_ICMP_SLE); break;
        case OP_I32_LE_U: case OP_I64_LE_U: emit_cmp(ctx, LR_ICMP_ULE); break;
        case OP_I32_GE_S: case OP_I64_GE_S: emit_cmp(ctx, LR_ICMP_SGE); break;
        case OP_I32_GE_U: case OP_I64_GE_U: emit_cmp(ctx, LR_ICMP_UGE); break;

        case OP_I32_EQZ: {
            lr_type_t *t = NULL;
            uint32_t val = vpop(ctx, &t);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[2] = {
                lr_op_vreg(val, m->type_i32),
                lr_op_imm_i64(0, m->type_i32)
            };
            emit_icmp(ctx, m->type_i1, dest, ops, 2, LR_ICMP_EQ);
            vpush(ctx, dest, m->type_i32);
            break;
        }
        case OP_I64_EQZ: {
            uint32_t val = vpop(ctx, NULL);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[2] = {
                lr_op_vreg(val, m->type_i64),
                lr_op_imm_i64(0, m->type_i64)
            };
            emit_icmp(ctx, m->type_i1, dest, ops, 2, LR_ICMP_EQ);
            vpush(ctx, dest, m->type_i32);
            break;
        }

        case OP_I32_WRAP_I64: {
            uint32_t val = vpop(ctx, NULL);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[1] = { lr_op_vreg(val, m->type_i64) };
            emit_inst(ctx, LR_OP_TRUNC, m->type_i32, dest, ops, 1);
            vpush(ctx, dest, m->type_i32);
            break;
        }
        case OP_I64_EXTEND_I32_S: {
            uint32_t val = vpop(ctx, NULL);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[1] = { lr_op_vreg(val, m->type_i32) };
            emit_inst(ctx, LR_OP_SEXT, m->type_i64, dest, ops, 1);
            vpush(ctx, dest, m->type_i64);
            break;
        }
        case OP_I64_EXTEND_I32_U: {
            uint32_t val = vpop(ctx, NULL);
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t ops[1] = { lr_op_vreg(val, m->type_i32) };
            emit_inst(ctx, LR_OP_ZEXT, m->type_i64, dest, ops, 1);
            vpush(ctx, dest, m->type_i64);
            break;
        }

        case OP_I32_LOAD: case OP_I64_LOAD:
        case OP_I32_LOAD8_S: case OP_I32_LOAD8_U:
        case OP_I32_LOAD16_S: case OP_I32_LOAD16_U: {
            /* memarg: align (ignored), offset */
            body_u32(body, body_len, &pos); /* align */
            uint32_t offset = body_u32(body, body_len, &pos);
            uint32_t addr = vpop(ctx, NULL);
            /* addr + offset -> effective address */
            uint32_t eff_addr = addr;
            if (offset != 0) {
                eff_addr = lr_vreg_new(ctx->func);
                lr_operand_t add_ops[2] = {
                    lr_op_vreg(addr, m->type_i32),
                    lr_op_imm_i64(offset, m->type_i32)
                };
                emit_inst(ctx, LR_OP_ADD, m->type_i32, eff_addr, add_ops, 2);
            }
            /* Convert addr to ptr via inttoptr */
            uint32_t ptr = lr_vreg_new(ctx->func);
            lr_operand_t conv_ops[1] = { lr_op_vreg(eff_addr, m->type_i32) };
            emit_inst(ctx, LR_OP_INTTOPTR, m->type_ptr, ptr, conv_ops, 1);
            /* Load */
            lr_type_t *load_type = (op == OP_I64_LOAD) ? m->type_i64 : m->type_i32;
            uint32_t dest = lr_vreg_new(ctx->func);
            lr_operand_t load_ops[1] = { lr_op_vreg(ptr, m->type_ptr) };
            emit_inst(ctx, LR_OP_LOAD, load_type, dest, load_ops, 1);
            vpush(ctx, dest, load_type);
            break;
        }
        case OP_I32_STORE: case OP_I64_STORE:
        case OP_I32_STORE8: case OP_I32_STORE16: {
            body_u32(body, body_len, &pos); /* align */
            uint32_t offset = body_u32(body, body_len, &pos);
            lr_type_t *val_type = NULL;
            uint32_t val = vpop(ctx, &val_type);
            uint32_t addr = vpop(ctx, NULL);
            uint32_t eff_addr = addr;
            if (offset != 0) {
                eff_addr = lr_vreg_new(ctx->func);
                lr_operand_t add_ops[2] = {
                    lr_op_vreg(addr, m->type_i32),
                    lr_op_imm_i64(offset, m->type_i32)
                };
                emit_inst(ctx, LR_OP_ADD, m->type_i32, eff_addr, add_ops, 2);
            }
            uint32_t ptr = lr_vreg_new(ctx->func);
            lr_operand_t conv_ops[1] = { lr_op_vreg(eff_addr, m->type_i32) };
            emit_inst(ctx, LR_OP_INTTOPTR, m->type_ptr, ptr, conv_ops, 1);
            lr_operand_t store_ops[2] = {
                lr_op_vreg(val, val_type),
                lr_op_vreg(ptr, m->type_ptr)
            };
            uint32_t dummy = lr_vreg_new(ctx->func);
            emit_inst(ctx, LR_OP_STORE, m->type_void, dummy, store_ops, 2);
            break;
        }

        default:
            /* Unknown opcode */
            snprintf(ctx->err, ctx->errlen, "unsupported WASM opcode 0x%02X", op);
            ctx->failed = true;
            return;
        }
    }

    /* Emit return in the function exit block (implicit at end of WASM function) */
    if (ctx->func->ret_type->kind == LR_TYPE_VOID) {
        lr_operand_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        emit_inst(ctx, LR_OP_RET_VOID, m->type_void, 0, &dummy, 0);
    } else {
        /* If there's a value on the stack, return it */
        if (ctx->vstack.top > 0) {
            lr_type_t *t = NULL;
            uint32_t val = vpop(ctx, &t);
            lr_operand_t ops[1] = { lr_op_vreg(val, t) };
            emit_inst(ctx, LR_OP_RET, m->type_void, 0, ops, 1);
        } else {
            lr_operand_t ops[1] = { lr_op_imm_i64(0, ctx->func->ret_type) };
            emit_inst(ctx, LR_OP_RET, m->type_void, 0, ops, 1);
        }
    }
}

/* ---- Module-level conversion ---- */

static lr_module_t *wasm_build_module_streaming(const lr_wasm_module_t *wmod,
                                                lr_arena_t *arena,
                                                lr_wasm_inst_callback_t on_inst,
                                                void *on_inst_ctx,
                                                char *err, size_t errlen) {
    lr_module_t *m;
    if (!wmod || !arena) {
        if (err && errlen > 0)
            snprintf(err, errlen, "invalid wasm conversion input");
        return NULL;
    }
    m = lr_module_create(arena);
    if (!m) {
        if (err && errlen > 0)
            snprintf(err, errlen, "failed to allocate liric module");
        return NULL;
    }

    /* Create IR functions for imports (as declarations) */
    for (uint32_t i = 0; i < wmod->num_imports; i++) {
        if (wmod->imports[i].kind != 0) continue; /* only func imports */
        uint32_t tidx = wmod->imports[i].type_idx;
        if (tidx >= wmod->num_types) {
            snprintf(err, errlen, "import type index out of range");
            return NULL;
        }
        const lr_wasm_functype_t *ft = &wmod->types[tidx];
        lr_type_t *ret = (ft->num_results > 0) ?
            wasm_to_lr_type(m, ft->results[0]) : m->type_void;
        lr_type_t **params = lr_arena_array(arena, lr_type_t *, ft->num_params);
        for (uint32_t j = 0; j < ft->num_params; j++)
            params[j] = wasm_to_lr_type(m, ft->params[j]);
        lr_func_declare(m, wmod->imports[i].name, ret, params, ft->num_params, false);
    }

    /* Create IR functions for local (defined) functions */
    for (uint32_t i = 0; i < wmod->num_funcs; i++) {
        uint32_t tidx = wmod->func_type_indices[i];
        if (tidx >= wmod->num_types) {
            snprintf(err, errlen, "func type index out of range");
            return NULL;
        }
        const lr_wasm_functype_t *ft = &wmod->types[tidx];
        lr_type_t *ret = (ft->num_results > 0) ?
            wasm_to_lr_type(m, ft->results[0]) : m->type_void;
        lr_type_t **params = lr_arena_array(arena, lr_type_t *, ft->num_params);
        for (uint32_t j = 0; j < ft->num_params; j++)
            params[j] = wasm_to_lr_type(m, ft->params[j]);

        /* Find the export name for this function, if any */
        const char *name = NULL;
        uint32_t abs_idx = wmod->num_func_imports + i;
        for (uint32_t e = 0; e < wmod->num_exports; e++) {
            if (wmod->exports[e].kind == 0 && wmod->exports[e].index == abs_idx) {
                name = wmod->exports[e].name;
                break;
            }
        }
        if (!name) {
            char buf[32];
            snprintf(buf, sizeof(buf), "__wasm_func_%u", i);
            name = lr_arena_strdup(arena, buf, strlen(buf));
        }

        lr_func_t *func = lr_func_create(m, name, ret, params, ft->num_params, false);

        /* Create entry block */
        lr_block_create(func, arena, "entry");
    }

    /* Convert each function body */
    lr_func_t *func = m->first_func;
    /* Skip import declarations */
    for (uint32_t i = 0; i < wmod->num_func_imports; i++) {
        if (func) func = func->next;
    }
    for (uint32_t i = 0; i < wmod->num_funcs && i < wmod->num_codes; i++) {
        if (!func) break;
        wasm_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.mod = m;
        ctx.arena = arena;
        ctx.func = func;
        ctx.cur_block = func->first_block;
        ctx.err = err;
        ctx.errlen = errlen;
        ctx.on_inst = on_inst;
        ctx.on_inst_ctx = on_inst_ctx;

        convert_func_body(&ctx, wmod, i);
        if (ctx.failed) return NULL;

        func = func->next;
    }

    return m;
}

lr_module_t *lr_wasm_build_module(const lr_wasm_module_t *wmod,
                                  lr_arena_t *arena,
                                  char *err, size_t errlen) {
    return wasm_build_module_streaming(wmod, arena, NULL, NULL, err, errlen);
}

static void session_err_set(lr_error_t *err, int code, const char *msg) {
    if (!err)
        return;
    err->code = code;
    if (msg)
        snprintf(err->msg, sizeof(err->msg), "%s", msg);
    else
        err->msg[0] = '\0';
}

static lr_type_t *map_type_to_session(lr_session_t *session,
                                      const lr_type_t *src_type) {
    if (!session || !src_type)
        return NULL;
    switch (src_type->kind) {
    case LR_TYPE_VOID: return lr_type_void_s(session);
    case LR_TYPE_I1: return lr_type_i1_s(session);
    case LR_TYPE_I8: return lr_type_i8_s(session);
    case LR_TYPE_I16: return lr_type_i16_s(session);
    case LR_TYPE_I32: return lr_type_i32_s(session);
    case LR_TYPE_I64: return lr_type_i64_s(session);
    case LR_TYPE_FLOAT: return lr_type_f32_s(session);
    case LR_TYPE_DOUBLE: return lr_type_f64_s(session);
    case LR_TYPE_PTR: return lr_type_ptr_s(session);
    default:
        return NULL;
    }
}

static lr_operand_desc_t map_operand_to_session(const lr_operand_t *src_op,
                                                lr_session_t *session,
                                                const lr_module_t *src_mod,
                                                const uint32_t *func_sym_ids,
                                                uint32_t func_sym_count) {
    lr_operand_desc_t out;
    memset(&out, 0, sizeof(out));
    if (!src_op || !session)
        return out;
    out.type = map_type_to_session(session, src_op->type);
    out.global_offset = src_op->global_offset;
    switch (src_op->kind) {
    case LR_VAL_VREG:
        out.kind = LR_OP_KIND_VREG;
        out.vreg = src_op->vreg;
        break;
    case LR_VAL_IMM_I64:
        out.kind = LR_OP_KIND_IMM_I64;
        out.imm_i64 = src_op->imm_i64;
        break;
    case LR_VAL_IMM_F64:
        out.kind = LR_OP_KIND_IMM_F64;
        out.imm_f64 = src_op->imm_f64;
        break;
    case LR_VAL_BLOCK:
        out.kind = LR_OP_KIND_BLOCK;
        out.block_id = src_op->block_id;
        break;
    case LR_VAL_GLOBAL: {
        uint32_t mapped = src_op->global_id;
        const char *sym_name = NULL;
        out.kind = LR_OP_KIND_GLOBAL;
        if (mapped < func_sym_count && func_sym_ids) {
            uint32_t sid = func_sym_ids[mapped];
            if (sid != UINT32_MAX)
                mapped = sid;
        } else if (src_mod) {
            sym_name = lr_module_symbol_name(src_mod, mapped);
            if (sym_name) {
                uint32_t sid = lr_session_intern(session, sym_name);
                if (sid != UINT32_MAX)
                    mapped = sid;
            }
        }
        out.global_id = mapped;
        break;
    }
    case LR_VAL_NULL:
        out.kind = LR_OP_KIND_NULL;
        break;
    case LR_VAL_UNDEF:
    default:
        out.kind = LR_OP_KIND_UNDEF;
        break;
    }
    return out;
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

static int replay_function_to_session(const lr_module_t *src_mod,
                                      const lr_func_t *src_func,
                                      lr_session_t *session,
                                      const uint32_t *func_sym_ids,
                                      uint32_t func_sym_count,
                                      void **out_addr,
                                      lr_error_t *err) {
    lr_type_t **params = NULL;
    lr_type_t *ret_type = NULL;
    int rc = 0;
    uint32_t i;
    const lr_block_t *block;

    if (out_addr)
        *out_addr = NULL;
    if (!src_mod || !src_func || !session) {
        session_err_set(err, LR_ERR_ARGUMENT, "invalid replay arguments");
        return -1;
    }

    ret_type = map_type_to_session(session, src_func->ret_type);
    if (!ret_type) {
        session_err_set(err, LR_ERR_PARSE, "unsupported wasm return type");
        return -1;
    }
    if (src_func->num_params > 0) {
        params = (lr_type_t **)calloc(src_func->num_params, sizeof(*params));
        if (!params) {
            session_err_set(err, LR_ERR_BACKEND, "param allocation failed");
            return -1;
        }
        for (i = 0; i < src_func->num_params; i++) {
            params[i] = map_type_to_session(session, src_func->param_types[i]);
            if (!params[i]) {
                free(params);
                session_err_set(err, LR_ERR_PARSE, "unsupported wasm param type");
                return -1;
            }
        }
    }

    rc = lr_session_func_begin(session, src_func->name, ret_type, params,
                               src_func->num_params, src_func->vararg, err);
    free(params);
    if (rc != 0)
        return -1;

    for (i = 0; i < src_func->num_blocks; i++) {
        uint32_t block_id = lr_session_block(session);
        if (block_id != i) {
            session_err_set(err, LR_ERR_STATE, "session block allocation mismatch");
            return -1;
        }
    }

    for (block = src_func->first_block; block; block = block->next) {
        const lr_inst_t *inst;
        rc = lr_session_set_block(session, block->id, err);
        if (rc != 0)
            return -1;

        for (inst = block->first; inst; inst = inst->next) {
            lr_inst_desc_t desc;
            lr_operand_desc_t *ops = NULL;
            lr_error_t emit_err = {0};
            uint32_t emit_dest;

            memset(&desc, 0, sizeof(desc));
            desc.op = inst->op;
            desc.type = map_type_to_session(session, inst->type);
            desc.dest = inst->dest;
            desc.num_operands = inst->num_operands;
            desc.num_indices = inst->num_indices;
            desc.indices = inst->indices;
            desc.icmp_pred = inst->icmp_pred;
            desc.fcmp_pred = inst->fcmp_pred;
            desc.call_external_abi = inst->call_external_abi;
            desc.call_vararg = inst->call_vararg;
            desc.call_fixed_args = inst->call_fixed_args;

            if (desc.num_operands > 0) {
                uint32_t j;
                ops = (lr_operand_desc_t *)calloc(desc.num_operands, sizeof(*ops));
                if (!ops) {
                    session_err_set(err, LR_ERR_BACKEND, "operand allocation failed");
                    return -1;
                }
                for (j = 0; j < desc.num_operands; j++) {
                    ops[j] = map_operand_to_session(&inst->operands[j], session,
                                                    src_mod, func_sym_ids,
                                                    func_sym_count);
                }
                desc.operands = ops;
            }

            emit_dest = lr_session_emit(session, &desc, &emit_err);
            free(ops);
            if (emit_err.code != LR_OK) {
                if (err)
                    *err = emit_err;
                return -1;
            }
            if (opcode_has_dest(desc.op, desc.type) &&
                desc.dest != 0 &&
                emit_dest != desc.dest) {
                session_err_set(err, LR_ERR_BACKEND, "vreg replay mismatch");
                return -1;
            }
        }
    }

    rc = lr_session_func_end(session, out_addr, err);
    if (rc != 0)
        return -1;
    return 0;
}

int lr_wasm_to_session(const lr_wasm_module_t *wmod,
                       lr_session_t *session,
                       void **out_last_addr,
                       lr_error_t *err) {
    lr_arena_t *tmp_arena = NULL;
    lr_module_t *tmp_mod = NULL;
    lr_func_t *func;
    uint32_t *func_sym_ids = NULL;
    uint32_t func_count = 0;
    uint32_t idx = 0;
    char parse_err[256] = {0};
    void *last_addr = NULL;

    if (out_last_addr)
        *out_last_addr = NULL;
    if (err) {
        err->code = LR_OK;
        err->msg[0] = '\0';
    }
    if (!wmod || !session) {
        session_err_set(err, LR_ERR_ARGUMENT, "invalid wasm session conversion input");
        return -1;
    }

    tmp_arena = lr_arena_create(0);
    if (!tmp_arena) {
        session_err_set(err, LR_ERR_BACKEND, "arena allocation failed");
        return -1;
    }
    tmp_mod = wasm_build_module_streaming(wmod, tmp_arena, NULL, NULL,
                                          parse_err, sizeof(parse_err));
    if (!tmp_mod) {
        lr_arena_destroy(tmp_arena);
        session_err_set(err, LR_ERR_PARSE, parse_err[0] ? parse_err
                                                        : "wasm to module conversion failed");
        return -1;
    }

    for (func = tmp_mod->first_func; func; func = func->next)
        func_count++;
    if (func_count > 0) {
        func_sym_ids = (uint32_t *)calloc(func_count, sizeof(*func_sym_ids));
        if (!func_sym_ids) {
            lr_arena_destroy(tmp_arena);
            session_err_set(err, LR_ERR_BACKEND, "symbol map allocation failed");
            return -1;
        }
        idx = 0;
        for (func = tmp_mod->first_func; func; func = func->next) {
            func_sym_ids[idx++] = lr_session_intern(session, func->name);
        }
    }

    idx = 0;
    for (func = tmp_mod->first_func; func; func = func->next, idx++) {
        lr_type_t **params = NULL;
        lr_type_t *ret_type = map_type_to_session(session, func->ret_type);
        int rc;

        if (!ret_type) {
            free(func_sym_ids);
            lr_arena_destroy(tmp_arena);
            session_err_set(err, LR_ERR_PARSE, "unsupported wasm return type");
            return -1;
        }
        if (func->num_params > 0) {
            uint32_t i;
            params = (lr_type_t **)calloc(func->num_params, sizeof(*params));
            if (!params) {
                free(func_sym_ids);
                lr_arena_destroy(tmp_arena);
                session_err_set(err, LR_ERR_BACKEND, "param allocation failed");
                return -1;
            }
            for (i = 0; i < func->num_params; i++) {
                params[i] = map_type_to_session(session, func->param_types[i]);
                if (!params[i]) {
                    free(params);
                    free(func_sym_ids);
                    lr_arena_destroy(tmp_arena);
                    session_err_set(err, LR_ERR_PARSE, "unsupported wasm param type");
                    return -1;
                }
            }
        }

        if (func->is_decl) {
            rc = lr_session_declare(session, func->name, ret_type, params,
                                    func->num_params, func->vararg, err);
            free(params);
            if (rc != 0) {
                free(func_sym_ids);
                lr_arena_destroy(tmp_arena);
                return -1;
            }
            continue;
        }
        free(params);

        if (replay_function_to_session(tmp_mod, func, session, func_sym_ids,
                                       func_count, &last_addr, err) != 0) {
            free(func_sym_ids);
            lr_arena_destroy(tmp_arena);
            return -1;
        }
    }

    if (out_last_addr)
        *out_last_addr = last_addr;
    free(func_sym_ids);
    lr_arena_destroy(tmp_arena);
    return 0;
}
