#ifndef LIRIC_H
#define LIRIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Opaque handles ---------------------------------------------------- */

typedef struct lr_module lr_module_t;
typedef struct lr_func lr_func_t;
typedef struct lr_block lr_block_t;
typedef struct lr_type lr_type_t;
typedef struct lr_global lr_global_t;
typedef struct lr_jit lr_jit_t;

/* ---- Frontend: text / binary parsers ----------------------------------- */

lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_bc(const uint8_t *data, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_wasm(const uint8_t *data, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_auto(const uint8_t *data, size_t len, char *err, size_t errlen);

/* ---- Module lifecycle -------------------------------------------------- */

lr_module_t *lr_module_create_new(void);
void lr_module_free(lr_module_t *m);
void lr_module_dump_to(lr_module_t *m, void *file_handle);

/* ---- Type constructors ------------------------------------------------- */

lr_type_t *lr_type_void_get(lr_module_t *m);
lr_type_t *lr_type_i1_get(lr_module_t *m);
lr_type_t *lr_type_i8_get(lr_module_t *m);
lr_type_t *lr_type_i16_get(lr_module_t *m);
lr_type_t *lr_type_i32_get(lr_module_t *m);
lr_type_t *lr_type_i64_get(lr_module_t *m);
lr_type_t *lr_type_float_get(lr_module_t *m);
lr_type_t *lr_type_double_get(lr_module_t *m);
lr_type_t *lr_type_ptr_get(lr_module_t *m);
lr_type_t *lr_type_array_new(lr_module_t *m, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_struct_new(lr_module_t *m, lr_type_t **fields,
                               uint32_t num_fields, bool packed);
lr_type_t *lr_type_func_new(lr_module_t *m, lr_type_t *ret,
                              lr_type_t **params, uint32_t num_params,
                              bool vararg);

/* ---- Function / block / vreg ------------------------------------------- */

lr_func_t *lr_func_define(lr_module_t *m, const char *name, lr_type_t *ret,
                           lr_type_t **params, uint32_t num_params, bool vararg);
lr_func_t *lr_func_declare_ext(lr_module_t *m, const char *name, lr_type_t *ret,
                                lr_type_t **params, uint32_t num_params,
                                bool vararg);
uint32_t lr_func_param_vreg(lr_func_t *f, uint32_t param_idx);
uint32_t lr_func_num_params(lr_func_t *f);

lr_block_t *lr_block_new(lr_func_t *f, lr_module_t *m, const char *name);
uint32_t lr_block_id(lr_block_t *b);

uint32_t lr_vreg_alloc(lr_func_t *f);

/* ---- Global variables -------------------------------------------------- */

lr_global_t *lr_global_define(lr_module_t *m, const char *name, lr_type_t *type,
                               bool is_const, const void *init_data,
                               size_t init_size);
lr_global_t *lr_global_declare_ext(lr_module_t *m, const char *name,
                                    lr_type_t *type);
uint32_t lr_global_id(lr_global_t *g);
void lr_global_add_reloc(lr_module_t *m, lr_global_t *g, size_t offset,
                          const char *symbol_name);

/* ---- Symbol interning (for global/function references) ----------------- */

uint32_t lr_symbol_intern(lr_module_t *m, const char *name);

/* ---- Operand descriptors ----------------------------------------------- */

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

#define LR_VREG(v, t)    ((lr_operand_desc_t){ .kind = LR_OP_KIND_VREG, .vreg = (v), .type = (t) })
#define LR_IMM(v, t)     ((lr_operand_desc_t){ .kind = LR_OP_KIND_IMM_I64, .imm_i64 = (v), .type = (t) })
#define LR_IMM_F(v, t)   ((lr_operand_desc_t){ .kind = LR_OP_KIND_IMM_F64, .imm_f64 = (v), .type = (t) })
#define LR_BLOCK(id)     ((lr_operand_desc_t){ .kind = LR_OP_KIND_BLOCK, .block_id = (id), .type = NULL })
#define LR_GLOBAL(id, t) ((lr_operand_desc_t){ .kind = LR_OP_KIND_GLOBAL, .global_id = (id), .type = (t) })
#define LR_NULL(t)       ((lr_operand_desc_t){ .kind = LR_OP_KIND_NULL, .type = (t) })
#define LR_UNDEF(t)      ((lr_operand_desc_t){ .kind = LR_OP_KIND_UNDEF, .type = (t) })

/* Comparison predicates */
enum {
    LR_CMP_EQ = 0, LR_CMP_NE,
    LR_CMP_SGT, LR_CMP_SGE, LR_CMP_SLT, LR_CMP_SLE,
    LR_CMP_UGT, LR_CMP_UGE, LR_CMP_ULT, LR_CMP_ULE,
};
enum {
    LR_FCMP_FALSE = 0,
    LR_FCMP_OEQ, LR_FCMP_OGT, LR_FCMP_OGE, LR_FCMP_OLT, LR_FCMP_OLE,
    LR_FCMP_ONE, LR_FCMP_ORD,
    LR_FCMP_UEQ, LR_FCMP_UGT, LR_FCMP_UGE, LR_FCMP_ULT, LR_FCMP_ULE,
    LR_FCMP_UNE, LR_FCMP_UNO,
    LR_FCMP_TRUE,
};

/* ---- JIT --------------------------------------------------------------- */

lr_jit_t *lr_jit_create(void);
lr_jit_t *lr_jit_create_for_target(const char *target_name);
const char *lr_jit_host_target_name(void);
const char *lr_jit_target_name(const lr_jit_t *j);
void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr);
int lr_jit_load_library(lr_jit_t *j, const char *path);
void lr_jit_begin_update(lr_jit_t *j);
int lr_jit_add_module(lr_jit_t *j, lr_module_t *m);
void lr_jit_end_update(lr_jit_t *j);
void *lr_jit_get_function(lr_jit_t *j, const char *name);
void lr_jit_destroy(lr_jit_t *j);

#ifdef __cplusplus
}
#endif

#endif
