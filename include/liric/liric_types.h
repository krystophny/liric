#ifndef LIRIC_TYPES_H
#define LIRIC_TYPES_H

#include <liric/liric_legacy.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum lr_type_kind_public {
    LR_TYPE_VOID,
    LR_TYPE_I1,
    LR_TYPE_I8,
    LR_TYPE_I16,
    LR_TYPE_I32,
    LR_TYPE_I64,
    LR_TYPE_FLOAT,
    LR_TYPE_DOUBLE,
    LR_TYPE_X86_FP80,
    LR_TYPE_PTR,
    LR_TYPE_ARRAY,
    LR_TYPE_VECTOR,
    LR_TYPE_STRUCT,
    LR_TYPE_FUNC,
};

struct lr_type {
    enum lr_type_kind_public kind;
    union {
        struct { struct lr_type *elem; uint64_t count; } array;
        struct { struct lr_type **fields; uint32_t num_fields; bool packed; char *name; } struc;
        struct { struct lr_type *ret; struct lr_type **params; uint32_t num_params; bool vararg; } func;
    };
};

struct lr_inst {
    int op;
    lr_type_t *type;
    uint32_t dest;
    void *operands;
    uint32_t num_operands;
    union {
        int icmp_pred;
        int fcmp_pred;
        uint32_t *indices;
    };
    uint32_t num_indices;
    bool call_external_abi;
    bool call_vararg;
    uint32_t call_fixed_args;
    struct lr_inst *next;
};

struct lr_block {
    char *name;
    uint32_t id;
    struct lr_inst *first;
    struct lr_inst *last;
    struct lr_inst **inst_array;
    uint32_t num_insts;
    struct lr_func *func;
    struct lr_block *next;
};

struct lr_func {
    char *name;
    lr_type_t *type;
    lr_type_t *ret_type;
    lr_type_t **param_types;
    uint32_t num_params;
    uint32_t *param_vregs;
    bool vararg;
    bool is_decl;
    bool uses_llvm_abi;
    lr_block_t *first_block;
    lr_block_t *last_block;
    lr_block_t **block_array;
    struct lr_inst **linear_inst_array;
    uint32_t *block_inst_offsets;
    uint32_t num_linear_insts;
    uint32_t num_blocks;
    uint32_t next_vreg;
    struct lr_func *next;
};

struct lr_reloc {
    size_t offset;
    char *symbol_name;
    struct lr_reloc *next;
};

struct lr_global {
    char *name;
    lr_type_t *type;
    uint8_t *init_data;
    size_t init_size;
    struct lr_reloc *relocs;
    bool is_const;
    bool is_external;
    bool is_local;
    uint32_t id;
    struct lr_global *next;
};

/*
 * lr_arena is opaque to C++ consumers. Only lr_arena_alloc/lr_arena_strdup
 * are needed, and they take lr_arena_t* as an opaque pointer.
 */
#ifndef __cplusplus
typedef struct lr_arena_chunk {
    struct lr_arena_chunk *next;
    size_t size;
    size_t used;
    uint8_t data[];
} lr_arena_chunk_t;

struct lr_arena {
    lr_arena_chunk_t *head;
    size_t default_chunk_size;
};
#else
struct lr_arena;
#endif

typedef struct lr_arena lr_arena_t;

void *lr_arena_alloc(lr_arena_t *a, size_t size, size_t align);
void *lr_arena_alloc_uninit(lr_arena_t *a, size_t size, size_t align);
char *lr_arena_strdup(lr_arena_t *a, const char *s, size_t len);

struct lr_module {
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
    lr_type_t *type_x86_fp80;
    lr_type_t *type_ptr;
    void *obj_ctx;
};

lr_func_t *lr_func_declare(lr_module_t *m, const char *name, lr_type_t *ret,
                            lr_type_t **params, uint32_t num_params, bool vararg);
uint32_t lr_module_intern_symbol(lr_module_t *m, const char *name);

#ifdef __cplusplus
}
#endif

#endif
