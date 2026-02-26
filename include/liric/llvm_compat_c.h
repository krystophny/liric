#ifndef LIRIC_LLVM_COMPAT_C_H
#define LIRIC_LLVM_COMPAT_C_H

#include <liric/liric_compat.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lr_llvm_compat_object lr_llvm_compat_object_t;
typedef struct lr_llvm_compat_dwarf_context lr_llvm_compat_dwarf_context_t;
typedef struct lr_llvm_compat_dwarf_unit lr_llvm_compat_dwarf_unit_t;

typedef struct lr_llvm_compat_dwarf_row {
    int end_sequence;
    uint64_t line;
    uint64_t file;
    uint64_t address;
    uint64_t section_index;
} lr_llvm_compat_dwarf_row_t;

typedef struct lr_llvm_compat_vector_type_info {
    const lr_type_t *element;
    unsigned num_elements;
    int scalable;
} lr_llvm_compat_vector_type_info_t;

enum {
    LR_LLVM_COMPAT_FILELINE_ABSOLUTE = 0
};

/* Generic C-side registry for C++ wrapper associations. */
void lr_llvm_compat_register_value_wrapper(const void *obj, lc_value_t *value);
lc_value_t *lr_llvm_compat_lookup_value_wrapper(const void *obj);
void lr_llvm_compat_unregister_value_wrapper(const void *obj);

void lr_llvm_compat_register_function_wrapper(const lr_func_t *func, void *fn_wrapper);
void *lr_llvm_compat_lookup_function_wrapper(const lr_func_t *func);
void lr_llvm_compat_unregister_function_wrapper(const lr_func_t *func);

void lr_llvm_compat_register_block_parent(const lr_block_t *block, void *fn_wrapper);
void *lr_llvm_compat_lookup_block_parent(const lr_block_t *block);
void lr_llvm_compat_unregister_block_parent(const lr_block_t *block);
void lr_llvm_compat_unregister_blocks_for_function(void *fn_wrapper);

void lr_llvm_compat_register_type_context(const lr_type_t *ty, const void *ctx);
const void *lr_llvm_compat_lookup_type_context(const lr_type_t *ty);
void lr_llvm_compat_register_vector_type(const lr_type_t *ty,
                                         const lr_type_t *element,
                                         unsigned num_elements,
                                         int scalable);
int lr_llvm_compat_lookup_vector_type(const lr_type_t *ty,
                                      lr_llvm_compat_vector_type_info_t *out_info);
void lr_llvm_compat_unregister_type_contexts(const void *ctx);

void lr_llvm_compat_register_global_alias(const lc_module_compat_t *mod,
                                          const char *logical_name,
                                          const char *actual_name);
int lr_llvm_compat_lookup_global_alias(const lc_module_compat_t *mod,
                                       const char *logical_name,
                                       char *out_name,
                                       size_t out_name_cap,
                                       size_t *out_name_len);
void lr_llvm_compat_clear_global_aliases(const lc_module_compat_t *mod);

void lr_llvm_compat_global_value_set_linkage(const void *obj, int linkage);
int lr_llvm_compat_global_value_get_linkage(const void *obj, int *out_linkage);
void lr_llvm_compat_global_value_set_visibility(const void *obj, int visibility);
int lr_llvm_compat_global_value_get_visibility(const void *obj, int *out_visibility);
void lr_llvm_compat_global_value_set_unnamed_addr(const void *obj, int unnamed_addr);
int lr_llvm_compat_global_value_get_unnamed_addr(const void *obj, int *out_unnamed_addr);
void lr_llvm_compat_unregister_global_value_state(const void *obj);

/* Generic helpers used by thin C++ compat wrappers. */
int lr_llvm_compat_is_local_global_linkage(int linkage);
size_t lr_llvm_compat_linkage_scoped_global_name(const lc_module_compat_t *compat,
                                                 const char *name,
                                                 int linkage,
                                                 char *out_name,
                                                 size_t out_name_cap);
void lr_llvm_compat_apply_global_linkage(lc_module_compat_t *compat,
                                         lc_value_t *global_value,
                                         int linkage);
const char *lr_llvm_compat_type_kind_name(unsigned type_kind);
size_t lr_llvm_compat_cstr_len(const char *s);
int lr_llvm_compat_bytes_equal(const void *lhs, const void *rhs, size_t n);

size_t lr_llvm_compat_intrinsic_name(unsigned intrinsic_id,
                                     int overload_is_float,
                                     int overload_is_double,
                                     int overload_is_integer,
                                     unsigned overload_int_bits,
                                     unsigned powi_int_bits,
                                     char *out_name,
                                     size_t out_name_cap);

int lr_llvm_compat_block_erase(lr_block_t *block, uint32_t *out_removed_id);
int lr_llvm_compat_block_move_after(lr_block_t *block, lr_block_t *anchor);
int lr_llvm_compat_block_move_before(lr_block_t *block, lr_block_t *anchor);

unsigned lr_llvm_compat_function_block_count(const lr_func_t *func);
int lr_llvm_compat_function_insert_block(lc_module_compat_t *mod,
                                         lr_func_t *func,
                                         lr_block_t *block,
                                         lr_block_t *insert_before);

int lr_llvm_compat_object_create(const char *path,
                                 lr_llvm_compat_object_t **out_obj);
void lr_llvm_compat_object_destroy(lr_llvm_compat_object_t *obj);
size_t lr_llvm_compat_object_section_count(const lr_llvm_compat_object_t *obj);
int lr_llvm_compat_object_section_get(const lr_llvm_compat_object_t *obj,
                                      size_t index,
                                      uint64_t *out_addr,
                                      uint64_t *out_size,
                                      uint64_t *out_index);

int lr_llvm_compat_dwarf_context_create(const lr_llvm_compat_object_t *obj,
                                        lr_llvm_compat_dwarf_context_t **out_ctx);
void lr_llvm_compat_dwarf_context_destroy(lr_llvm_compat_dwarf_context_t *ctx);
size_t lr_llvm_compat_dwarf_context_unit_count(const lr_llvm_compat_dwarf_context_t *ctx);
const lr_llvm_compat_dwarf_unit_t *lr_llvm_compat_dwarf_context_unit_at(
    const lr_llvm_compat_dwarf_context_t *ctx, size_t index);
const char *lr_llvm_compat_dwarf_unit_compilation_dir(const lr_llvm_compat_dwarf_unit_t *unit);

size_t lr_llvm_compat_dwarf_line_row_count(const lr_llvm_compat_dwarf_context_t *ctx,
                                           const lr_llvm_compat_dwarf_unit_t *unit);
int lr_llvm_compat_dwarf_line_row_get(const lr_llvm_compat_dwarf_context_t *ctx,
                                      const lr_llvm_compat_dwarf_unit_t *unit,
                                      size_t row_index,
                                      lr_llvm_compat_dwarf_row_t *out_row);
int lr_llvm_compat_dwarf_line_has_file_index(const lr_llvm_compat_dwarf_context_t *ctx,
                                             const lr_llvm_compat_dwarf_unit_t *unit,
                                             uint64_t file_index);
int lr_llvm_compat_dwarf_line_get_file_name(const lr_llvm_compat_dwarf_context_t *ctx,
                                            const lr_llvm_compat_dwarf_unit_t *unit,
                                            uint64_t file_index,
                                            const char *comp_dir,
                                            int file_line_kind,
                                            char *out_name,
                                            size_t out_name_cap,
                                            size_t *out_name_len);

int lr_llvm_compat_symbolize_code(const char *binary_path,
                                  uint64_t address,
                                  uint64_t section_index,
                                  int demangle,
                                  char *out_file,
                                  size_t out_file_cap,
                                  char *out_func,
                                  size_t out_func_cap,
                                  uint32_t *out_line);

#ifdef __cplusplus
}
#endif

#endif
