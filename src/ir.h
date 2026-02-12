#ifndef LIRIC_IR_H
#define LIRIC_IR_H

#include "arena.h"
#include <stdbool.h>
#include <stdio.h>

typedef enum lr_type_kind {
    LR_TYPE_VOID,
    LR_TYPE_I1,
    LR_TYPE_I8,
    LR_TYPE_I16,
    LR_TYPE_I32,
    LR_TYPE_I64,
    LR_TYPE_FLOAT,
    LR_TYPE_DOUBLE,
    LR_TYPE_PTR,
    LR_TYPE_ARRAY,
    LR_TYPE_STRUCT,
    LR_TYPE_FUNC,
} lr_type_kind_t;

typedef struct lr_type {
    lr_type_kind_t kind;
    union {
        struct { struct lr_type *elem; uint64_t count; } array;
        struct { struct lr_type **fields; uint32_t num_fields; bool packed; char *name; } struc;
        struct { struct lr_type *ret; struct lr_type **params; uint32_t num_params; bool vararg; } func;
    };
} lr_type_t;

typedef enum lr_opcode {
    LR_OP_RET,
    LR_OP_RET_VOID,
    LR_OP_BR,
    LR_OP_CONDBR,
    LR_OP_UNREACHABLE,
    LR_OP_ADD,
    LR_OP_SUB,
    LR_OP_MUL,
    LR_OP_SDIV,
    LR_OP_SREM,
    LR_OP_AND,
    LR_OP_OR,
    LR_OP_XOR,
    LR_OP_SHL,
    LR_OP_LSHR,
    LR_OP_ASHR,
    LR_OP_FADD,
    LR_OP_FSUB,
    LR_OP_FMUL,
    LR_OP_FDIV,
    LR_OP_FNEG,
    LR_OP_ICMP,
    LR_OP_FCMP,
    LR_OP_ALLOCA,
    LR_OP_LOAD,
    LR_OP_STORE,
    LR_OP_GEP,
    LR_OP_CALL,
    LR_OP_PHI,
    LR_OP_SELECT,
    LR_OP_SEXT,
    LR_OP_ZEXT,
    LR_OP_TRUNC,
    LR_OP_BITCAST,
    LR_OP_PTRTOINT,
    LR_OP_INTTOPTR,
    LR_OP_SITOFP,
    LR_OP_UITOFP,
    LR_OP_FPTOSI,
    LR_OP_FPTOUI,
    LR_OP_FPEXT,
    LR_OP_FPTRUNC,
    LR_OP_EXTRACTVALUE,
    LR_OP_INSERTVALUE,
} lr_opcode_t;

typedef enum lr_icmp_pred {
    LR_ICMP_EQ, LR_ICMP_NE,
    LR_ICMP_SGT, LR_ICMP_SGE, LR_ICMP_SLT, LR_ICMP_SLE,
    LR_ICMP_UGT, LR_ICMP_UGE, LR_ICMP_ULT, LR_ICMP_ULE,
} lr_icmp_pred_t;

typedef enum lr_fcmp_pred {
    LR_FCMP_FALSE,
    LR_FCMP_OEQ, LR_FCMP_OGT, LR_FCMP_OGE, LR_FCMP_OLT, LR_FCMP_OLE, LR_FCMP_ONE, LR_FCMP_ORD,
    LR_FCMP_UEQ, LR_FCMP_UGT, LR_FCMP_UGE, LR_FCMP_ULT, LR_FCMP_ULE, LR_FCMP_UNE, LR_FCMP_UNO,
    LR_FCMP_TRUE,
} lr_fcmp_pred_t;

typedef enum lr_operand_kind {
    LR_VAL_VREG,
    LR_VAL_IMM_I64,
    LR_VAL_IMM_F64,
    LR_VAL_BLOCK,
    LR_VAL_GLOBAL,
    LR_VAL_NULL,
    LR_VAL_UNDEF,
} lr_operand_kind_t;

typedef struct lr_operand {
    lr_operand_kind_t kind;
    lr_type_t *type;
    int64_t global_offset;
    union {
        uint32_t vreg;
        int64_t imm_i64;
        double imm_f64;
        uint32_t block_id;
        uint32_t global_id;
    };
} lr_operand_t;

typedef struct lr_inst {
    lr_opcode_t op;
    lr_type_t *type;
    uint32_t dest;
    lr_operand_t *operands;
    uint32_t num_operands;
    union {
        lr_icmp_pred_t icmp_pred;
        lr_fcmp_pred_t fcmp_pred;
        uint32_t *indices;
    };
    uint32_t num_indices;
    bool call_external_abi;
    bool call_vararg;
    struct lr_inst *next;
} lr_inst_t;

typedef struct lr_phi_copy {
    uint32_t dest_vreg;
    lr_operand_t src_op;
} lr_phi_copy_t;

typedef struct lr_block_phi_copies {
    lr_phi_copy_t *copies;
    uint32_t count;
} lr_block_phi_copies_t;

typedef struct lr_gep_step {
    bool is_const;
    int64_t const_byte_offset;
    size_t runtime_elem_size;
    uint8_t runtime_signext_bytes;
    const lr_type_t *next_type;
} lr_gep_step_t;

typedef struct lr_block {
    char *name;
    uint32_t id;
    lr_inst_t *first;
    lr_inst_t *last;
    lr_inst_t **inst_array;
    uint32_t num_insts;
    struct lr_func *func;
    struct lr_block *next;
} lr_block_t;

typedef struct lr_func {
    char *name;
    lr_type_t *type;
    lr_type_t *ret_type;
    lr_type_t **param_types;
    uint32_t num_params;
    uint32_t *param_vregs;
    bool vararg;
    bool is_decl;
    lr_block_t *first_block;
    lr_block_t *last_block;
    lr_block_t **block_array;
    lr_inst_t **linear_inst_array;
    uint32_t *block_inst_offsets;
    uint32_t num_linear_insts;
    uint32_t num_blocks;
    uint32_t next_vreg;
    struct lr_func *next;
} lr_func_t;

typedef struct lr_reloc {
    size_t offset;
    int64_t addend;
    char *symbol_name;
    struct lr_reloc *next;
} lr_reloc_t;

typedef struct lr_global {
    char *name;
    lr_type_t *type;
    uint8_t *init_data;
    size_t init_size;
    lr_reloc_t *relocs;
    bool is_const;
    bool is_external;
    uint32_t id;
    struct lr_global *next;
} lr_global_t;

typedef struct lr_module {
    lr_arena_t *arena;
    lr_func_t *first_func;
    lr_func_t *last_func;
    lr_global_t *first_global;
    lr_global_t *last_global;
    uint32_t num_globals;
    char **symbol_names;
    uint32_t *symbol_hashes;
    uint32_t num_symbols;
    uint32_t symbol_cap;
    uint32_t *symbol_index;
    uint32_t symbol_index_cap;
    lr_type_t *type_void;
    lr_type_t *type_i1;
    lr_type_t *type_i8;
    lr_type_t *type_i16;
    lr_type_t *type_i32;
    lr_type_t *type_i64;
    lr_type_t *type_float;
    lr_type_t *type_double;
    lr_type_t *type_ptr;
    void *obj_ctx;
} lr_module_t;

lr_module_t *lr_module_create(lr_arena_t *arena);
lr_type_t *lr_type_func(lr_arena_t *a, lr_type_t *ret, lr_type_t **params,
                         uint32_t num_params, bool vararg);
lr_type_t *lr_type_array(lr_arena_t *a, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_struct(lr_arena_t *a, lr_type_t **fields, uint32_t n,
                           bool packed, char *name);
lr_func_t *lr_func_create(lr_module_t *m, const char *name, lr_type_t *ret,
                           lr_type_t **params, uint32_t num_params, bool vararg);
lr_func_t *lr_func_declare(lr_module_t *m, const char *name, lr_type_t *ret,
                            lr_type_t **params, uint32_t num_params, bool vararg);
lr_block_t *lr_block_create(lr_func_t *f, lr_arena_t *a, const char *name);
uint32_t lr_vreg_new(lr_func_t *f);
lr_inst_t *lr_inst_create(lr_arena_t *a, lr_opcode_t op, lr_type_t *type,
                           uint32_t dest, lr_operand_t *ops, uint32_t nops);
void lr_block_append(lr_block_t *b, lr_inst_t *inst);
int lr_func_finalize(lr_func_t *f, lr_arena_t *a);
bool lr_func_is_finalized(const lr_func_t *f);
lr_global_t *lr_global_create(lr_module_t *m, const char *name, lr_type_t *type,
                               bool is_const);

lr_operand_t lr_op_vreg(uint32_t vreg, lr_type_t *type);
lr_operand_t lr_op_imm_i64(int64_t val, lr_type_t *type);
lr_operand_t lr_op_imm_f64(double val, lr_type_t *type);
lr_operand_t lr_op_block(uint32_t id);
lr_operand_t lr_op_global(uint32_t id, lr_type_t *type);
lr_operand_t lr_op_null(lr_type_t *type);
uint32_t lr_module_intern_symbol(lr_module_t *m, const char *name);
const char *lr_module_symbol_name(const lr_module_t *m, uint32_t id);

size_t lr_type_size(const lr_type_t *t);
size_t lr_type_align(const lr_type_t *t);
size_t lr_struct_field_offset(const lr_type_t *st, uint32_t field_idx);
bool lr_aggregate_index_path(const lr_type_t *base, const uint32_t *indices,
                             uint32_t num_indices, size_t *byte_offset_out,
                             const lr_type_t **leaf_type_out);
lr_block_phi_copies_t *lr_build_phi_copies(lr_arena_t *arena, lr_func_t *func);
uint8_t lr_gep_index_signext_bytes(const lr_operand_t *idx_op);
bool lr_gep_analyze_step(const lr_type_t *cur_ty, bool first_index,
                         const lr_operand_t *idx_op, lr_gep_step_t *out);
lr_operand_t lr_canonicalize_gep_index(lr_module_t *m, lr_block_t *b,
                                       lr_func_t *f, lr_operand_t idx_op);

int lr_module_merge(lr_module_t *dest, lr_module_t *src);

void lr_module_dump(lr_module_t *m, FILE *out);

#endif
