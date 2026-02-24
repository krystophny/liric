#ifndef LIRIC_COMPAT_H
#define LIRIC_COMPAT_H

#include <liric/liric_legacy.h>
#include <liric/liric_types.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value kind tags */
typedef enum lc_value_kind {
    LC_VAL_VREG,
    LC_VAL_CONST_INT,
    LC_VAL_CONST_FP,
    LC_VAL_CONST_NULL,
    LC_VAL_CONST_UNDEF,
    LC_VAL_GLOBAL,
    LC_VAL_ARGUMENT,
    LC_VAL_BLOCK,
    LC_VAL_CONST_AGGREGATE,
} lc_value_kind_t;

/* Unified value handle wrapping all LLVM value types */
typedef struct lc_value {
    lc_value_kind_t kind;
    lr_type_t *type;
    union {
        struct { uint32_t id; lr_func_t *func; } vreg;
        struct { int64_t val; unsigned width; } const_int;
        struct { double val; bool is_double; } const_fp;
        struct { uint32_t id; const char *name; lr_func_t *func; int64_t offset; } global;
        struct { uint32_t param_idx; lr_func_t *func; } argument;
        struct { lr_block_t *block; } block;
        struct { const void *data; size_t size; } aggregate;
    };
} lc_value_t;

/* Context (lightweight - tracks the module for arena allocation) */
typedef struct lc_context {
    lr_module_t *mod;
    lr_type_t *type_void;
    lr_type_t *type_i1;
    lr_type_t *type_i8;
    lr_type_t *type_i16;
    lr_type_t *type_i32;
    lr_type_t *type_i64;
    lr_type_t *type_float;
    lr_type_t *type_double;
    lr_type_t *type_ptr;
    lr_arena_t *type_arena;
    int backend;
} lc_context_t;

/* Module wrapper */
typedef struct lc_module_compat {
    lr_module_t *mod;
    lc_context_t *ctx;
    const char *name;
    lc_value_t *value_pool;
    uint32_t value_count;
    uint32_t value_cap;
    lc_value_t **func_values;
    uint32_t func_value_count;
    uint32_t func_value_cap;
} lc_module_compat_t;

/* PHI node deferred state */
typedef struct lc_phi_node {
    lc_value_t *result;
    lr_type_t *type;
    lr_block_t *block;
    lr_func_t *func;
    lr_module_t *mod;
    lc_value_t **incoming_vals;
    uint32_t *incoming_block_ids;
    uint32_t num_incoming;
    uint32_t cap_incoming;
    bool finalized;
} lc_phi_node_t;

/* Alloca instruction handle */
typedef struct lc_alloca_inst {
    lc_value_t *result;
    lr_type_t *alloc_type;
} lc_alloca_inst_t;

typedef struct lc_switch_builder lc_switch_builder_t;

/* ---- Context ---- */
lc_context_t *lc_context_create(void);
void lc_context_destroy(lc_context_t *ctx);
void lc_context_set_backend(lc_context_t *ctx, int backend);
int lc_context_get_backend(const lc_context_t *ctx);

enum {
    LC_BACKEND_DEFAULT = 0,
    LC_BACKEND_ISEL = 1,
    LC_BACKEND_COPY_PATCH = 2,
    LC_BACKEND_LLVM = 3,
};

/* ---- Module ---- */
lc_module_compat_t *lc_module_create(lc_context_t *ctx, const char *name);
void lc_module_destroy(lc_module_compat_t *mod);
lr_module_t *lc_module_get_ir(lc_module_compat_t *mod);
void lc_module_dump(lc_module_compat_t *mod);
void lc_module_print(lc_module_compat_t *mod, FILE *out);
char *lc_module_sprint(lc_module_compat_t *mod, size_t *out_len);

/* ---- Value allocation ---- */
lc_value_t *lc_value_alloc(lc_module_compat_t *mod);
lc_value_t *lc_value_vreg(lc_module_compat_t *mod, uint32_t id,
                           lr_type_t *type, lr_func_t *func);
lc_value_t *lc_value_const_int(lc_module_compat_t *mod, lr_type_t *type,
                                int64_t val, unsigned width);
lc_value_t *lc_value_const_fp(lc_module_compat_t *mod, lr_type_t *type,
                               double val, bool is_double);
lc_value_t *lc_value_const_null(lc_module_compat_t *mod, lr_type_t *type);
lc_value_t *lc_value_undef(lc_module_compat_t *mod, lr_type_t *type);
lc_value_t *lc_value_global(lc_module_compat_t *mod, uint32_t id,
                             lr_type_t *type, const char *name);
lc_value_t *lc_value_global_with_addend(lc_module_compat_t *mod,
                                         lc_value_t *base,
                                         int64_t addend);
lc_value_t *lc_value_argument(lc_module_compat_t *mod, uint32_t param_idx,
                               lr_type_t *type, lr_func_t *func);
lc_value_t *lc_value_block_ref(lc_module_compat_t *mod, lr_block_t *block);
lc_value_t *lc_value_const_aggregate(lc_module_compat_t *mod, lr_type_t *type,
                                      const void *data, size_t size);
int lc_value_const_aggregate_add_reloc(lc_module_compat_t *mod,
                                        lc_value_t *aggregate,
                                        size_t offset,
                                        const char *symbol_name,
                                        int64_t addend);

/* Convert lc_value_t to lr_operand_desc_t for passing to builder functions */
lr_operand_desc_t lc_value_to_desc(lc_value_t *val);

/* ---- Type queries ---- */
lr_type_t *lc_get_int_type(lc_module_compat_t *mod, unsigned width);
lr_type_t *lc_get_void_type(lc_module_compat_t *mod);
lr_type_t *lc_get_float_type(lc_module_compat_t *mod);
lr_type_t *lc_get_double_type(lc_module_compat_t *mod);
lr_type_t *lc_get_ptr_type(lc_module_compat_t *mod);
bool lc_type_is_integer(lr_type_t *ty);
bool lc_type_is_floating(lr_type_t *ty);
bool lc_type_is_pointer(lr_type_t *ty);
unsigned lc_type_int_width(lr_type_t *ty);
unsigned lc_type_primitive_size_bits(lr_type_t *ty);
size_t lc_type_size_bits(lr_type_t *ty);
size_t lc_type_store_size(lr_type_t *ty);
size_t lc_type_alloc_size(lr_type_t *ty);
lr_type_t *lc_type_struct_field(lr_type_t *ty, unsigned idx);
lr_type_t *lc_type_contained(lr_type_t *ty, unsigned idx);
unsigned lc_type_struct_num_fields(lr_type_t *ty);
bool lc_type_struct_has_name(lr_type_t *ty);
lc_value_t *lc_global_lookup_or_create(lc_module_compat_t *mod,
                                        const char *name, lr_type_t *type);

/* ---- Compat utility helpers ---- */
const char *lc_intrinsic_name(unsigned intrinsic_id);
bool lc_is_lfortran_jit_wrapper_ir(const char *asm_text, size_t len);
lr_module_t *lc_build_lfortran_jit_wrapper_module(char *err, size_t errlen);
size_t lc_format_i64(char *buf, size_t buf_size, int64_t value);
size_t lc_format_u64(char *buf, size_t buf_size, uint64_t value);
size_t lc_format_f64(char *buf, size_t buf_size, double value);
size_t lc_format_ptr(char *buf, size_t buf_size, const void *ptr);
bool lc_pack_constant_bytes(lc_value_t *value, lr_type_t *ty,
                             uint8_t *out, size_t out_size);
lc_value_t *lc_const_struct_from_values(lc_module_compat_t *mod,
                                         lr_type_t *struct_ty,
                                         lc_value_t **values,
                                         uint32_t num_values);
lc_value_t *lc_const_array_from_values(lc_module_compat_t *mod,
                                        lr_type_t *array_ty,
                                        lc_value_t **values,
                                        uint32_t num_values);
int lc_const_gep_compute_offset(lr_type_t *base_type,
                                 lc_value_t **indices,
                                 uint32_t num_indices,
                                 int64_t *out_offset);

/* ---- Function ---- */
lc_value_t *lc_func_create(lc_module_compat_t *mod, const char *name,
                            lr_type_t *func_type);
lc_value_t *lc_func_declare(lc_module_compat_t *mod, const char *name,
                             lr_type_t *func_type);
lr_func_t *lc_value_get_func(lc_value_t *val);
lc_value_t *lc_func_get_arg(lc_module_compat_t *mod, lc_value_t *func_val,
                             unsigned idx);
unsigned lc_func_arg_count(lc_value_t *func_val);

/* ---- Basic block ---- */
lc_value_t *lc_block_create(lc_module_compat_t *mod, lr_func_t *func,
                             const char *name);
lc_value_t *lc_block_create_detached(lc_module_compat_t *mod, lr_func_t *func,
                                      const char *name);
int lc_block_attach(lc_module_compat_t *mod, lr_block_t *block);
lr_block_t *lc_value_get_block(lc_value_t *val);
lr_func_t *lc_value_get_block_func(lc_value_t *val);
lc_phi_node_t *lc_value_get_phi_node(lc_value_t *val);
lr_type_t *lc_value_get_alloca_type(lc_value_t *val);
bool lc_block_has_terminator(lr_block_t *block);
bool lc_func_uses_block_id(lr_func_t *func, lr_block_t *skip_block,
                            uint32_t block_id);
void lc_func_remap_block_operands_after_erase(lr_func_t *func,
                                               uint32_t removed_id);

/* ---- Value/switch compat helpers ---- */
int lc_value_replace_all_uses_with(lc_value_t *from, lc_value_t *to);
lc_switch_builder_t *lc_switch_builder_create(lc_module_compat_t *mod,
                                              lr_block_t *origin,
                                              lr_func_t *func,
                                              lc_value_t *cond,
                                              lr_block_t *default_block);
int lc_switch_builder_add_case(lc_switch_builder_t *sw,
                               lc_value_t *on_value,
                               lr_block_t *dest_block);
void lc_switch_builder_destroy(lc_switch_builder_t *sw);

/* ---- Global variable ---- */
lc_value_t *lc_global_create(lc_module_compat_t *mod, const char *name,
                              lr_type_t *type, bool is_const,
                              const void *init_data, size_t init_size);
lc_value_t *lc_global_declare(lc_module_compat_t *mod, const char *name,
                               lr_type_t *type);
lc_value_t *lc_global_lookup(lc_module_compat_t *mod, const char *name);
int lc_global_set_initializer(lc_module_compat_t *mod, lc_value_t *global_val,
                               lc_value_t *init_val);
bool lc_global_has_initializer(lc_module_compat_t *mod, lc_value_t *global_val);

/* ---- IRBuilder operations ---- */

/* Arithmetic */
lc_value_t *lc_create_add(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name);
lc_value_t *lc_create_sub(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name);
lc_value_t *lc_create_mul(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name);
lc_value_t *lc_create_sdiv(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_srem(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_udiv(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_urem(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_neg(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *val, const char *name);

/* Bitwise */
lc_value_t *lc_create_and(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name);
lc_value_t *lc_create_or(lc_module_compat_t *mod, lr_block_t *b,
                          lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                          const char *name);
lc_value_t *lc_create_xor(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name);
lc_value_t *lc_create_shl(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                           const char *name);
lc_value_t *lc_create_lshr(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_ashr(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_not(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lc_value_t *val, const char *name);

/* FP arithmetic */
lc_value_t *lc_create_fadd(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_fsub(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_fmul(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_fdiv(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *lhs, lc_value_t *rhs,
                            const char *name);
lc_value_t *lc_create_fneg(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *val, const char *name);

/* Comparison */
lc_value_t *lc_create_icmp_eq(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *lhs,
                               lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_ne(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *lhs,
                               lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_slt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_sle(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_sgt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_sge(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_ult(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_uge(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_ugt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_icmp_ule(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_oeq(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_one(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_olt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_ole(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_ogt(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_oge(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_une(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_ord(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_uno(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);
lc_value_t *lc_create_fcmp_ueq(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *lhs,
                                lc_value_t *rhs, const char *name);

/* Memory */
lc_alloca_inst_t *lc_create_alloca(lc_module_compat_t *mod, lr_block_t *b,
                                    lr_func_t *f, lr_type_t *type,
                                    lc_value_t *array_size, const char *name);
lc_value_t *lc_create_load(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lr_type_t *ty, lc_value_t *ptr,
                            const char *name);
void lc_create_store(lc_module_compat_t *mod, lr_block_t *b,
                     lc_value_t *val, lc_value_t *ptr);
lc_value_t *lc_create_gep(lc_module_compat_t *mod, lr_block_t *b,
                           lr_func_t *f, lr_type_t *base_type,
                           lc_value_t *base_ptr, lc_value_t **indices,
                           unsigned num_indices, const char *name);
lc_value_t *lc_create_inbounds_gep(lc_module_compat_t *mod, lr_block_t *b,
                                    lr_func_t *f, lr_type_t *base_type,
                                    lc_value_t *base_ptr,
                                    lc_value_t **indices,
                                    unsigned num_indices, const char *name);
lc_value_t *lc_create_struct_gep(lc_module_compat_t *mod, lr_block_t *b,
                                  lr_func_t *f, lr_type_t *base_type,
                                  lc_value_t *base_ptr, unsigned idx,
                                  const char *name);

/* Control flow */
void lc_create_ret(lc_module_compat_t *mod, lr_block_t *b, lc_value_t *val);
void lc_create_ret_void(lc_module_compat_t *mod, lr_block_t *b);
void lc_create_br(lc_module_compat_t *mod, lr_block_t *b,
                  lr_block_t *target);
void lc_create_cond_br(lc_module_compat_t *mod, lr_block_t *b,
                       lc_value_t *cond, lr_block_t *true_bb,
                       lr_block_t *false_bb);
void lc_create_unreachable(lc_module_compat_t *mod, lr_block_t *b);

/* Calls */
lc_value_t *lc_create_call(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lr_type_t *func_type,
                            lc_value_t *callee, lc_value_t **args,
                            unsigned num_args, const char *name);

/* PHI */
lc_phi_node_t *lc_create_phi(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lr_type_t *ty, const char *name);
void lc_phi_add_incoming(lc_phi_node_t *phi, lc_value_t *val,
                         lr_block_t *block);
void lc_phi_finalize(lc_phi_node_t *phi);

/* Select */
lc_value_t *lc_create_select(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *cond,
                              lc_value_t *true_val, lc_value_t *false_val,
                              const char *name);

/* Type conversions */
lc_value_t *lc_create_sext(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *val,
                            lr_type_t *to_type, const char *name);
lc_value_t *lc_create_zext(lc_module_compat_t *mod, lr_block_t *b,
                            lr_func_t *f, lc_value_t *val,
                            lr_type_t *to_type, const char *name);
lc_value_t *lc_create_trunc(lc_module_compat_t *mod, lr_block_t *b,
                             lr_func_t *f, lc_value_t *val,
                             lr_type_t *to_type, const char *name);
lc_value_t *lc_create_bitcast(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *val,
                               lr_type_t *to_type, const char *name);
lc_value_t *lc_create_ptrtoint(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *val,
                                lr_type_t *to_type, const char *name);
lc_value_t *lc_create_inttoptr(lc_module_compat_t *mod, lr_block_t *b,
                                lr_func_t *f, lc_value_t *val,
                                lr_type_t *to_type, const char *name);
lc_value_t *lc_create_sitofp(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name);
lc_value_t *lc_create_uitofp(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name);
lc_value_t *lc_create_fptosi(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name);
lc_value_t *lc_create_fptoui(lc_module_compat_t *mod, lr_block_t *b,
                              lr_func_t *f, lc_value_t *val,
                              lr_type_t *to_type, const char *name);
lc_value_t *lc_create_fpext(lc_module_compat_t *mod, lr_block_t *b,
                             lr_func_t *f, lc_value_t *val,
                             lr_type_t *to_type, const char *name);
lc_value_t *lc_create_fptrunc(lc_module_compat_t *mod, lr_block_t *b,
                               lr_func_t *f, lc_value_t *val,
                               lr_type_t *to_type, const char *name);
lc_value_t *lc_create_sext_or_trunc(lc_module_compat_t *mod, lr_block_t *b,
                                     lr_func_t *f, lc_value_t *val,
                                     lr_type_t *to_type, const char *name);
lc_value_t *lc_create_zext_or_trunc(lc_module_compat_t *mod, lr_block_t *b,
                                     lr_func_t *f, lc_value_t *val,
                                     lr_type_t *to_type, const char *name);

/* Aggregate */
lc_value_t *lc_create_extractvalue(lc_module_compat_t *mod, lr_block_t *b,
                                    lr_func_t *f, lc_value_t *agg,
                                    unsigned *indices, unsigned num_indices,
                                    const char *name);
lc_value_t *lc_create_insertvalue(lc_module_compat_t *mod, lr_block_t *b,
                                   lr_func_t *f, lc_value_t *agg,
                                   lc_value_t *val, unsigned *indices,
                                   unsigned num_indices, const char *name);

/* MemCpy / MemSet (emit as call to libc) */
void lc_create_memcpy(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                      lc_value_t *dst, lc_value_t *src, lc_value_t *size);
void lc_create_memset(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                      lc_value_t *dst, lc_value_t *val, lc_value_t *size);
void lc_create_memmove(lc_module_compat_t *mod, lr_block_t *b, lr_func_t *f,
                       lc_value_t *dst, lc_value_t *src, lc_value_t *size);

/* ---- Direct JIT materialization ---- */
int lc_module_add_to_jit(lc_module_compat_t *mod, lr_jit_t *jit);
int lc_module_finalize_for_execution(lc_module_compat_t *mod);
void *lc_module_lookup_in_session(lc_module_compat_t *mod, const char *name);
void lc_module_add_external_symbol(lc_module_compat_t *mod, const char *name,
                                   void *addr);
int lc_module_load_library(lc_module_compat_t *mod, const char *path);

/* ---- In-process JIT execution ---- */

/* JIT-compile the module and execute the named function (typically "main")
   via function pointer. Resolves runtime symbols from libraries loaded via
   lc_module_load_library or LIRIC_RUNTIME_LIB.  Eliminates all disk I/O,
   object emission, and system linker invocation.
   Returns the integer result of the called function, or -1 on error. */
int lc_module_jit_exec(lc_module_compat_t *mod, const char *entry_name);

/* ---- Object file emission ---- */
int lc_module_emit_object(lc_module_compat_t *mod, const char *filename);
int lc_module_emit_object_to_file(lc_module_compat_t *mod, FILE *out);
int lc_module_emit_executable(lc_module_compat_t *mod, const char *filename,
                              const char *runtime_ll, size_t runtime_len);
int lc_module_export_blob_package(lc_module_compat_t *mod,
                                  uint8_t **out_data, size_t *out_len);
int lc_module_import_blob_package(lc_module_compat_t *mod,
                                  const uint8_t *data, size_t len);
int lc_module_merge_ll_text(lc_module_compat_t *mod,
                            const char *src, size_t len);

/* ---- Session compat layer (LLVM-style naming) ---- */

typedef struct LLVMLiricSessionState LLVMLiricSessionState;
typedef LLVMLiricSessionState *LLVMLiricSessionStateRef;

LLVMLiricSessionStateRef LLVMLiricSessionCreate(void);
void LLVMLiricSessionDispose(LLVMLiricSessionStateRef state);
int LLVMLiricSessionAddCompatModule(LLVMLiricSessionStateRef state,
                                    lc_module_compat_t *mod);
void LLVMLiricSessionAddSymbol(LLVMLiricSessionStateRef state,
                               const char *name, void *addr);
int LLVMLiricSessionLoadLibrary(LLVMLiricSessionStateRef state,
                                const char *path);
void *LLVMLiricSessionLookup(LLVMLiricSessionStateRef state, const char *name);
const char *LLVMLiricHostTargetName(void);

#ifdef __cplusplus
}
#endif

#endif
