#ifndef LIRIC_LEGACY_H
#define LIRIC_LEGACY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <liric/liric_ir_shared.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Legacy low-level API retained for internal plumbing and LLVM C++ compat.
 * New integrations should use <liric/liric.h> unified lr_compiler_* API.
 */

typedef struct lr_module lr_module_t;
typedef struct lr_func lr_func_t;
typedef struct lr_block lr_block_t;
typedef struct lr_type lr_type_t;
typedef struct lr_global lr_global_t;
typedef struct lr_jit lr_jit_t;

typedef int (*lr_ll_func_cb_t)(lr_func_t *func, lr_module_t *mod, void *ctx);

/* Frontend parsers */
lr_module_t *lr_parse_ll(const char *src, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_ll_streaming(const char *src, size_t len,
                                   lr_ll_func_cb_t on_func, void *ctx,
                                   char *err, size_t errlen);
lr_module_t *lr_parse_bc(const uint8_t *data, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_wasm(const uint8_t *data, size_t len, char *err, size_t errlen);
lr_module_t *lr_parse_auto(const uint8_t *data, size_t len, char *err, size_t errlen);

/* Module lifecycle */
void lr_module_free(lr_module_t *m);
int lr_module_merge(lr_module_t *dest, lr_module_t *src);

/* Composite type constructors */
lr_type_t *lr_type_array_new(lr_module_t *m, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_vector_new(lr_module_t *m, lr_type_t *elem, uint64_t count);
lr_type_t *lr_type_struct_new(lr_module_t *m, lr_type_t **fields,
                              uint32_t num_fields, bool packed);
lr_type_t *lr_type_func_new(lr_module_t *m, lr_type_t *ret,
                            lr_type_t **params, uint32_t num_params,
                            bool vararg);

#define LR_VREG(v, t) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_VREG, .vreg = (v), .type = (t), .global_offset = 0 })
#define LR_IMM(v, t) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_IMM_I64, .imm_i64 = (v), .type = (t), .global_offset = 0 })
#define LR_IMM_F(v, t) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_IMM_F64, .imm_f64 = (v), .type = (t), .global_offset = 0 })
#define LR_BLOCK(id) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_BLOCK, .block_id = (id), .type = NULL, .global_offset = 0 })
#define LR_GLOBAL(id, t) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_GLOBAL, .global_id = (id), .type = (t), .global_offset = 0 })
#define LR_NULL(t) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_NULL, .type = (t), .global_offset = 0 })
#define LR_UNDEF(t) \
    ((lr_operand_desc_t){ .kind = LR_OP_KIND_UNDEF, .type = (t), .global_offset = 0 })

/* Comparison predicates */
enum {
    LR_CMP_EQ = 0, LR_CMP_NE,
    LR_CMP_SGT, LR_CMP_SGE, LR_CMP_SLT, LR_CMP_SLE,
    LR_CMP_UGT, LR_CMP_UGE, LR_CMP_ULT, LR_CMP_ULE,
};

/* Low-level JIT API */
lr_jit_t *lr_jit_create(void);
lr_jit_t *lr_jit_create_for_target(const char *target_name);
const char *lr_jit_host_target_name(void);
const char *lr_jit_target_name(const lr_jit_t *j);
void lr_jit_add_symbol(lr_jit_t *j, const char *name, void *addr);
int lr_jit_load_library(lr_jit_t *j, const char *path);
int lr_jit_set_runtime_bc(lr_jit_t *j, const uint8_t *bc_data, size_t bc_len);
void lr_jit_begin_update(lr_jit_t *j);
int lr_jit_add_module(lr_jit_t *j, lr_module_t *m);
void lr_jit_end_update(lr_jit_t *j);
void *lr_jit_get_function(lr_jit_t *j, const char *name);
void lr_jit_destroy(lr_jit_t *j);

#ifdef __cplusplus
}
#endif

#endif
