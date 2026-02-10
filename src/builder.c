#include "ir.h"
#include "arena.h"
#include <string.h>
#include <stdio.h>

/* Import the public operand descriptor type and its kind constants.
   We cannot include liric.h alongside ir.h due to enum redeclarations,
   so we replicate the minimal set needed here. */
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
    LR_OP_KIND_VREG    = 0,
    LR_OP_KIND_IMM_I64 = 1,
    LR_OP_KIND_IMM_F64 = 2,
    LR_OP_KIND_BLOCK   = 3,
    LR_OP_KIND_GLOBAL  = 4,
    LR_OP_KIND_NULL    = 5,
    LR_OP_KIND_UNDEF   = 6,
};

static lr_operand_t desc_to_op(lr_operand_desc_t d) {
    lr_operand_t op;
    op.kind = (lr_operand_kind_t)d.kind;
    op.type = d.type;
    op.global_offset = 0;
    op.imm_i64 = 0;
    switch (d.kind) {
    case LR_OP_KIND_VREG:    op.vreg = d.vreg; break;
    case LR_OP_KIND_IMM_I64: op.imm_i64 = d.imm_i64; break;
    case LR_OP_KIND_IMM_F64: op.imm_f64 = d.imm_f64; break;
    case LR_OP_KIND_BLOCK:   op.block_id = d.block_id; break;
    case LR_OP_KIND_GLOBAL:  op.global_id = d.global_id; break;
    case LR_OP_KIND_NULL:    break;
    case LR_OP_KIND_UNDEF:   break;
    }
    return op;
}

static uint32_t build_binop(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                             lr_opcode_t op, lr_type_t *ty,
                             lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { desc_to_op(lhs), desc_to_op(rhs) };
    lr_inst_t *inst = lr_inst_create(m->arena, op, ty, dest, ops, 2);
    lr_block_append(b, inst);
    return dest;
}

static uint32_t build_cast(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                            lr_opcode_t op, lr_type_t *to_type,
                            lr_operand_desc_t val) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { desc_to_op(val) };
    lr_inst_t *inst = lr_inst_create(m->arena, op, to_type, dest, ops, 1);
    lr_block_append(b, inst);
    return dest;
}

/* ---- Module lifecycle -------------------------------------------------- */

lr_module_t *lr_module_create_new(void) {
    lr_arena_t *arena = lr_arena_create(0);
    if (!arena) return NULL;
    return lr_module_create(arena);
}

void lr_module_dump_to(lr_module_t *m, void *file_handle) {
    lr_module_dump(m, (FILE *)file_handle);
}

/* ---- Type constructors ------------------------------------------------- */

lr_type_t *lr_type_void_get(lr_module_t *m)   { return m->type_void; }
lr_type_t *lr_type_i1_get(lr_module_t *m)     { return m->type_i1; }
lr_type_t *lr_type_i8_get(lr_module_t *m)     { return m->type_i8; }
lr_type_t *lr_type_i16_get(lr_module_t *m)    { return m->type_i16; }
lr_type_t *lr_type_i32_get(lr_module_t *m)    { return m->type_i32; }
lr_type_t *lr_type_i64_get(lr_module_t *m)    { return m->type_i64; }
lr_type_t *lr_type_float_get(lr_module_t *m)  { return m->type_float; }
lr_type_t *lr_type_double_get(lr_module_t *m) { return m->type_double; }
lr_type_t *lr_type_ptr_get(lr_module_t *m)    { return m->type_ptr; }

lr_type_t *lr_type_array_new(lr_module_t *m, lr_type_t *elem, uint64_t count) {
    return lr_type_array(m->arena, elem, count);
}

lr_type_t *lr_type_struct_new(lr_module_t *m, lr_type_t **fields,
                               uint32_t num_fields, bool packed) {
    return lr_type_struct(m->arena, fields, num_fields, packed, NULL);
}

lr_type_t *lr_type_func_new(lr_module_t *m, lr_type_t *ret,
                              lr_type_t **params, uint32_t num_params,
                              bool vararg) {
    return lr_type_func(m->arena, ret, params, num_params, vararg);
}

/* ---- Function / block / vreg ------------------------------------------- */

lr_func_t *lr_func_define(lr_module_t *m, const char *name, lr_type_t *ret,
                           lr_type_t **params, uint32_t num_params, bool vararg) {
    return lr_func_create(m, name, ret, params, num_params, vararg);
}

lr_func_t *lr_func_declare_ext(lr_module_t *m, const char *name, lr_type_t *ret,
                                lr_type_t **params, uint32_t num_params,
                                bool vararg) {
    return lr_func_declare(m, name, ret, params, num_params, vararg);
}

uint32_t lr_func_param_vreg(lr_func_t *f, uint32_t param_idx) {
    return f->param_vregs[param_idx];
}

uint32_t lr_func_num_params(lr_func_t *f) {
    return f->num_params;
}

lr_block_t *lr_block_new(lr_func_t *f, lr_module_t *m, const char *name) {
    return lr_block_create(f, m->arena, name);
}

uint32_t lr_block_id(lr_block_t *b) {
    return b->id;
}

uint32_t lr_vreg_alloc(lr_func_t *f) {
    return lr_vreg_new(f);
}

/* ---- Global variables -------------------------------------------------- */

lr_global_t *lr_global_define(lr_module_t *m, const char *name, lr_type_t *type,
                               bool is_const, const void *init_data,
                               size_t init_size) {
    lr_global_t *g = lr_global_create(m, name, type, is_const);
    if (init_data && init_size > 0) {
        g->init_data = lr_arena_alloc(m->arena, init_size, 1);
        memcpy(g->init_data, init_data, init_size);
        g->init_size = init_size;
    }
    return g;
}

lr_global_t *lr_global_declare_ext(lr_module_t *m, const char *name,
                                    lr_type_t *type) {
    lr_global_t *g = lr_global_create(m, name, type, false);
    g->is_external = true;
    return g;
}

uint32_t lr_global_id(lr_global_t *g) {
    return g->id;
}

void lr_global_add_reloc(lr_module_t *m, lr_global_t *g, size_t offset,
                          const char *symbol_name) {
    lr_reloc_t *r = lr_arena_new(m->arena, lr_reloc_t);
    r->offset = offset;
    r->symbol_name = lr_arena_strdup(m->arena, symbol_name, strlen(symbol_name));
    r->next = g->relocs;
    g->relocs = r;
}

/* ---- Symbol interning -------------------------------------------------- */

uint32_t lr_symbol_intern(lr_module_t *m, const char *name) {
    return lr_module_intern_symbol(m, name);
}

/* ---- Arithmetic -------------------------------------------------------- */

uint32_t lr_build_add(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_ADD, ty, lhs, rhs);
}

uint32_t lr_build_sub(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_SUB, ty, lhs, rhs);
}

uint32_t lr_build_mul(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_MUL, ty, lhs, rhs);
}

uint32_t lr_build_sdiv(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_SDIV, ty, lhs, rhs);
}

uint32_t lr_build_srem(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_SREM, ty, lhs, rhs);
}

/* ---- Bitwise ----------------------------------------------------------- */

uint32_t lr_build_and(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_AND, ty, lhs, rhs);
}

uint32_t lr_build_or(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                      lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_OR, ty, lhs, rhs);
}

uint32_t lr_build_xor(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_XOR, ty, lhs, rhs);
}

uint32_t lr_build_shl(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_SHL, ty, lhs, rhs);
}

uint32_t lr_build_lshr(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_LSHR, ty, lhs, rhs);
}

uint32_t lr_build_ashr(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_ASHR, ty, lhs, rhs);
}

/* ---- Floating-point arithmetic ----------------------------------------- */

uint32_t lr_build_fadd(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_FADD, ty, lhs, rhs);
}

uint32_t lr_build_fsub(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_FSUB, ty, lhs, rhs);
}

uint32_t lr_build_fmul(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_FMUL, ty, lhs, rhs);
}

uint32_t lr_build_fdiv(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    return build_binop(m, b, f, LR_OP_FDIV, ty, lhs, rhs);
}

uint32_t lr_build_fneg(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_FNEG, ty, val);
}

/* ---- Comparison -------------------------------------------------------- */

uint32_t lr_build_icmp(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        int pred, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { desc_to_op(lhs), desc_to_op(rhs) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_ICMP, m->type_i1, dest, ops, 2);
    inst->icmp_pred = (lr_icmp_pred_t)pred;
    lr_block_append(b, inst);
    return dest;
}

uint32_t lr_build_fcmp(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        int pred, lr_operand_desc_t lhs, lr_operand_desc_t rhs) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { desc_to_op(lhs), desc_to_op(rhs) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_FCMP, m->type_i1, dest, ops, 2);
    inst->fcmp_pred = (lr_fcmp_pred_t)pred;
    lr_block_append(b, inst);
    return dest;
}

/* ---- Memory ------------------------------------------------------------ */

uint32_t lr_build_alloca(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                          lr_type_t *elem_type) {
    uint32_t dest = lr_vreg_new(f);
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_ALLOCA,
                                      m->type_ptr, dest, NULL, 0);
    inst->type = elem_type;
    lr_block_append(b, inst);
    return dest;
}

uint32_t lr_build_alloca_array(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                                lr_type_t *elem_type, lr_operand_desc_t count) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { desc_to_op(count) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_ALLOCA,
                                      m->type_ptr, dest, ops, 1);
    inst->type = elem_type;
    lr_block_append(b, inst);
    return dest;
}

uint32_t lr_build_load(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ty, lr_operand_desc_t addr) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { desc_to_op(addr) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_LOAD, ty, dest, ops, 1);
    lr_block_append(b, inst);
    return dest;
}

void lr_build_store(lr_module_t *m, lr_block_t *b,
                     lr_operand_desc_t val, lr_operand_desc_t addr) {
    lr_operand_t ops[2] = { desc_to_op(val), desc_to_op(addr) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_STORE,
                                      m->type_void, 0, ops, 2);
    lr_block_append(b, inst);
}

uint32_t lr_build_gep(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *base_type, lr_operand_desc_t base_ptr,
                       lr_operand_desc_t *indices, uint32_t num_indices) {
    uint32_t dest = lr_vreg_new(f);
    uint32_t nops = 1 + num_indices;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = desc_to_op(base_ptr);
    for (uint32_t i = 0; i < num_indices; i++) {
        lr_operand_t idx = desc_to_op(indices[i]);
        ops[1 + i] = lr_canonicalize_gep_index(m, b, f, idx);
    }
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_GEP,
                                      m->type_ptr, dest, ops, nops);
    inst->type = base_type;
    lr_block_append(b, inst);
    return dest;
}

/* ---- Control flow ------------------------------------------------------ */

void lr_build_ret(lr_module_t *m, lr_block_t *b, lr_operand_desc_t val) {
    lr_operand_t ops[1] = { desc_to_op(val) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_RET,
                                      val.type, 0, ops, 1);
    lr_block_append(b, inst);
}

void lr_build_ret_void(lr_module_t *m, lr_block_t *b) {
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_RET_VOID,
                                      m->type_void, 0, NULL, 0);
    lr_block_append(b, inst);
}

void lr_build_br(lr_module_t *m, lr_block_t *b, uint32_t target_block_id) {
    lr_operand_t ops[1] = { lr_op_block(target_block_id) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_BR,
                                      m->type_void, 0, ops, 1);
    lr_block_append(b, inst);
}

void lr_build_condbr(lr_module_t *m, lr_block_t *b, lr_operand_desc_t cond,
                      uint32_t true_id, uint32_t false_id) {
    lr_operand_t ops[3] = {
        desc_to_op(cond),
        lr_op_block(true_id),
        lr_op_block(false_id)
    };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_CONDBR,
                                      m->type_void, 0, ops, 3);
    lr_block_append(b, inst);
}

void lr_build_unreachable(lr_module_t *m, lr_block_t *b) {
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_UNREACHABLE,
                                      m->type_void, 0, NULL, 0);
    lr_block_append(b, inst);
}

/* ---- Calls ------------------------------------------------------------- */

uint32_t lr_build_call(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *ret_type, lr_operand_desc_t callee,
                        lr_operand_desc_t *args, uint32_t num_args) {
    uint32_t dest = lr_vreg_new(f);
    uint32_t nops = 1 + num_args;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = desc_to_op(callee);
    for (uint32_t i = 0; i < num_args; i++)
        ops[1 + i] = desc_to_op(args[i]);
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_CALL,
                                      ret_type, dest, ops, nops);
    lr_block_append(b, inst);
    return dest;
}

void lr_build_call_void(lr_module_t *m, lr_block_t *b,
                         lr_operand_desc_t callee,
                         lr_operand_desc_t *args, uint32_t num_args) {
    uint32_t nops = 1 + num_args;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    ops[0] = desc_to_op(callee);
    for (uint32_t i = 0; i < num_args; i++)
        ops[1 + i] = desc_to_op(args[i]);
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_CALL,
                                      m->type_void, 0, ops, nops);
    lr_block_append(b, inst);
}

/* ---- PHI / select ------------------------------------------------------ */

uint32_t lr_build_phi(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                       lr_type_t *ty, lr_operand_desc_t *incoming_vals,
                       uint32_t *incoming_block_ids, uint32_t num_incoming) {
    uint32_t dest = lr_vreg_new(f);
    uint32_t nops = num_incoming * 2;
    lr_operand_t *ops = lr_arena_array(m->arena, lr_operand_t, nops);
    for (uint32_t i = 0; i < num_incoming; i++) {
        ops[i * 2]     = desc_to_op(incoming_vals[i]);
        ops[i * 2 + 1] = lr_op_block(incoming_block_ids[i]);
    }
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_PHI, ty, dest, ops, nops);
    lr_block_append(b, inst);
    return dest;
}

uint32_t lr_build_select(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                          lr_type_t *ty, lr_operand_desc_t cond,
                          lr_operand_desc_t true_val, lr_operand_desc_t false_val) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[3] = {
        desc_to_op(cond),
        desc_to_op(true_val),
        desc_to_op(false_val)
    };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_SELECT, ty, dest, ops, 3);
    lr_block_append(b, inst);
    return dest;
}

/* ---- Type conversions -------------------------------------------------- */

uint32_t lr_build_sext(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_SEXT, to_type, val);
}

uint32_t lr_build_zext(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                        lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_ZEXT, to_type, val);
}

uint32_t lr_build_trunc(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                         lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_TRUNC, to_type, val);
}

uint32_t lr_build_bitcast(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                           lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_BITCAST, to_type, val);
}

uint32_t lr_build_ptrtoint(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                            lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_PTRTOINT, to_type, val);
}

uint32_t lr_build_inttoptr(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                            lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_INTTOPTR, to_type, val);
}

uint32_t lr_build_sitofp(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                          lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_SITOFP, to_type, val);
}

uint32_t lr_build_fptosi(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                          lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_FPTOSI, to_type, val);
}

uint32_t lr_build_fpext(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                         lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_FPEXT, to_type, val);
}

uint32_t lr_build_fptrunc(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                           lr_type_t *to_type, lr_operand_desc_t val) {
    return build_cast(m, b, f, LR_OP_FPTRUNC, to_type, val);
}

/* ---- Aggregate --------------------------------------------------------- */

uint32_t lr_build_extractvalue(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                                lr_type_t *ty, lr_operand_desc_t agg,
                                uint32_t *indices, uint32_t num_indices) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[1] = { desc_to_op(agg) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_EXTRACTVALUE,
                                      ty, dest, ops, 1);
    inst->indices = lr_arena_array(m->arena, uint32_t, num_indices);
    memcpy(inst->indices, indices, sizeof(uint32_t) * num_indices);
    inst->num_indices = num_indices;
    lr_block_append(b, inst);
    return dest;
}

uint32_t lr_build_insertvalue(lr_module_t *m, lr_block_t *b, lr_func_t *f,
                               lr_type_t *ty, lr_operand_desc_t agg,
                               lr_operand_desc_t val,
                               uint32_t *indices, uint32_t num_indices) {
    uint32_t dest = lr_vreg_new(f);
    lr_operand_t ops[2] = { desc_to_op(agg), desc_to_op(val) };
    lr_inst_t *inst = lr_inst_create(m->arena, LR_OP_INSERTVALUE,
                                      ty, dest, ops, 2);
    inst->indices = lr_arena_array(m->arena, uint32_t, num_indices);
    memcpy(inst->indices, indices, sizeof(uint32_t) * num_indices);
    inst->num_indices = num_indices;
    lr_block_append(b, inst);
    return dest;
}
