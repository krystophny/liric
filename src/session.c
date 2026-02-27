#include "arena.h"
#include "bc_decode.h"
#include "compile_mode.h"
#include "ir.h"
#include "jit.h"
#include "liric.h"
#include "llvm_backend.h"
#include "module_emit.h"
#include "objfile.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

/* Session mode mirrors the public lr_session_mode_t. */
typedef enum session_mode {
    SESSION_MODE_DIRECT = 0,
    SESSION_MODE_IR = 1,
} session_mode_t;

typedef enum session_backend {
    SESSION_BACKEND_DEFAULT = 0,
    SESSION_BACKEND_ISEL = 1,
    SESSION_BACKEND_COPY_PATCH = 2,
    SESSION_BACKEND_LLVM = 3,
} session_backend_t;

/* Session config mirrors the public lr_session_config_t. */
typedef struct session_config {
    session_mode_t mode;
    const char *target;
    session_backend_t backend;
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

typedef struct direct_reloc_range {
    uint32_t start;
    uint32_t end;
} direct_reloc_range_t;

/* Saved state for a suspended direct-mode function compilation.
   When the compat layer switches from function A to function B mid-build,
   A's compile state is saved here so it can be resumed later. */
typedef struct suspended_compile {
    lr_func_t *func;
    lr_block_t *cur_block;
    lr_block_t **blocks;
    bool *block_seen;
    bool *block_terminated;
    uint32_t block_count;
    uint32_t block_cap;
    session_phi_copy_entry_t *phi_copies;
    uint32_t phi_copy_count;
    uint32_t phi_copy_cap;
    void *compile_ctx;
    uint8_t *func_buf;
    size_t func_buf_cap;
    uint32_t direct_reloc_base;
    direct_reloc_range_t *direct_reloc_ranges;
    uint32_t direct_reloc_range_count;
    uint32_t direct_reloc_range_cap;
    bool compile_active;
    bool compile_deferred;
    bool compile_opened_update;
    uint32_t emitted_count;
    uint8_t *null_derived;
    uint32_t null_derived_cap;
} suspended_compile_t;

struct lr_session {
    session_config_t cfg;
    lr_jit_t *jit;
    lr_module_t *owned_module;
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
    bool compile_deferred;   /* defer backend emission to function end */
    bool direct_llvm_stream;
    bool compile_opened_update;
    uint32_t emitted_count;

    /* Per-function temp buffer for direct mode compilation */
    uint8_t *func_compile_buf;
    size_t func_compile_buf_cap;

    /* Relocation ranges owned by the currently compiling function.
       A function can be suspended/resumed multiple times while other
       functions emit relocs into the shared obj_ctx. We capture each
       active [start,end) range so finalize/patch only touches owned relocs. */
    direct_reloc_range_t *direct_reloc_ranges;
    uint32_t direct_reloc_range_count;
    uint32_t direct_reloc_range_cap;
    uint32_t direct_reloc_active_start;
    bool direct_reloc_active;

    /* Suspended function compilations for interleaved generation */
    suspended_compile_t *suspended;
    uint32_t suspended_count;
    uint32_t suspended_cap;

    /* DIRECT mode blob capture for exe/obj emission */
    lr_objfile_ctx_t direct_obj_ctx;
    bool direct_obj_ctx_active;
    uint32_t direct_reloc_base;
    bool direct_pending_relocs;
    uint32_t direct_pending_reloc_start;
    lr_func_blob_t *blobs;
    uint32_t blob_count;
    uint32_t blob_cap;
    bool ir_module_jit_ready;
    bool jit_borrowed;  /* true = JIT owned externally, skip destroy */
    uint8_t *runtime_bc_data;
    size_t runtime_bc_len;
    bool runtime_bc_borrowed;  /* true = process-lifetime pointer, skip free */
    bool runtime_bc_registered_with_jit;
    bool runtime_bc_merged_into_main_module;

    /* Bitset tracking vregs known to hold null-derived values (e.g. GEP
       from null).  Used in the streaming compile path to skip backend
       emission of loads that would dereference null and crash, matching
       LLVM's ISel behavior which silently drops dead null loads. */
    uint8_t *null_derived;
    uint32_t null_derived_cap;
};

/* Derive the direct per-function compile buffer capacity from remaining JIT
   code space, so we do not rely on fixed compile-time buffer limits. */
static size_t direct_compile_buf_capacity(const struct lr_session *s) {
    if (!s || !s->jit || s->jit->code_cap <= s->jit->code_size)
        return 0;
    return s->jit->code_cap - s->jit->code_size;
}

static int ensure_block(struct lr_session *s, uint32_t block_id,
                        session_error_t *err);

static int register_owned_module(struct lr_session *s, lr_module_t *m,
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

static int register_owned_module(struct lr_session *s, lr_module_t *m,
                                 session_error_t *err) {
    lr_owned_module_t *node = NULL;
    if (!s || !m) {
        err_set(err, S_ERR_ARGUMENT, "invalid module ownership registration");
        return -1;
    }
    node = (lr_owned_module_t *)calloc(1, sizeof(*node));
    if (!node) {
        err_set(err, S_ERR_BACKEND, "module ownership registration failed");
        return -1;
    }
    node->module = m;
    node->next = s->owned_modules;
    s->owned_modules = node;
    return 0;
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

static void null_derived_mark(struct lr_session *s, uint32_t vreg) {
    if (vreg == 0 || vreg == UINT32_MAX)
        return;
    if (vreg >= s->null_derived_cap) {
        uint32_t new_cap = (vreg + 64u) & ~63u;
        uint8_t *p = (uint8_t *)realloc(s->null_derived, new_cap);
        if (!p)
            return;
        memset(p + s->null_derived_cap, 0, new_cap - s->null_derived_cap);
        s->null_derived = p;
        s->null_derived_cap = new_cap;
    }
    s->null_derived[vreg] = 1;
}

static bool null_derived_check(const struct lr_session *s, uint32_t vreg) {
    if (!s->null_derived || vreg >= s->null_derived_cap)
        return false;
    return s->null_derived[vreg] != 0;
}

static bool operand_is_null_derived(const struct lr_session *s,
                                     const lr_operand_desc_t *op) {
    if (op->kind == LR_OP_KIND_NULL)
        return true;
    if (op->kind == LR_OP_KIND_VREG)
        return null_derived_check(s, op->vreg);
    return false;
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

static const char *session_entry_symbol(const lr_module_t *m) {
    bool has_main = false;
    if (!m)
        return "_start";
    for (const lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->name || !f->name[0] || f->is_decl || !f->first_block)
            continue;
        if (strcmp(f->name, "_start") == 0)
            return "_start";
        if (strcmp(f->name, "main") == 0)
            has_main = true;
    }
    return has_main ? "main" : "_start";
}

static const char *session_blob_entry_symbol(const struct lr_session *s,
                                             const char *fallback) {
    const char *first = NULL;
    bool has_main = false;
    if (!s || !s->blobs || s->blob_count == 0)
        return fallback ? fallback : "_start";
    for (uint32_t i = 0; i < s->blob_count; i++) {
        const lr_func_blob_t *blob = &s->blobs[i];
        if (!blob->name || !blob->name[0] || !blob->code || blob->code_len == 0)
            continue;
        if (!first)
            first = blob->name;
        if (strcmp(blob->name, "_start") == 0)
            return "_start";
        if (strcmp(blob->name, "main") == 0)
            has_main = true;
    }
    if (has_main)
        return "main";
    return first ? first : (fallback ? fallback : "_start");
}

static int merge_runtime_bc_into_module(struct lr_session *s, lr_module_t *m,
                                        bool *merged_flag,
                                        session_error_t *err) {
    char parse_err[256] = {0};
    lr_module_t *rt = NULL;
    if (!s || !m || !s->runtime_bc_data || s->runtime_bc_len == 0)
        return 0;
    if (merged_flag && *merged_flag)
        return 0;
    rt = lr_parse_bc_with_arena(s->runtime_bc_data, s->runtime_bc_len,
                                m->arena, parse_err, sizeof(parse_err));
    if (!rt) {
        err_set(err, S_ERR_PARSE, "runtime bc parse failed: %s",
                parse_err[0] ? parse_err : "unknown parse error");
        return -1;
    }
    if (lr_module_merge(m, rt) != 0) {
        err_set(err, S_ERR_BACKEND, "runtime bc merge failed");
        return -1;
    }
    if (merged_flag)
        *merged_flag = true;
    return 0;
}

static int preload_runtime_bc_into_jit(struct lr_session *s,
                                       session_error_t *err) {
    lr_arena_t *arena = NULL;
    lr_module_t *rt = NULL;
    lr_owned_module_t *node = NULL;
    char parse_err[256] = {0};
    if (!s || !s->jit || !s->runtime_bc_data || s->runtime_bc_len == 0)
        return 0;
    if (s->runtime_bc_registered_with_jit)
        return 0;

    arena = lr_arena_create(s->runtime_bc_len * 3);
    if (!arena) {
        err_set(err, S_ERR_BACKEND, "runtime arena allocation failed");
        return -1;
    }
    rt = lr_parse_bc_streaming(s->runtime_bc_data, s->runtime_bc_len, arena,
                               NULL, NULL, parse_err, sizeof(parse_err));
    if (!rt) {
        lr_arena_destroy(arena);
        err_set(err, S_ERR_PARSE, "runtime bc parse failed: %s",
                parse_err[0] ? parse_err : "unknown parse error");
        return -1;
    }

    /* Avoid merging runtime into itself via jit add-module bootstrap. */
    s->jit->runtime_bc_loaded = true;
    if (lr_jit_add_module(s->jit, rt) != 0) {
        lr_module_free(rt);
        err_set(err, S_ERR_BACKEND, "runtime bc jit preload failed");
        return -1;
    }

    node = (lr_owned_module_t *)calloc(1, sizeof(*node));
    if (!node) {
        lr_module_free(rt);
        err_set(err, S_ERR_BACKEND, "runtime ownership registration failed");
        return -1;
    }
    node->module = rt;
    node->next = s->owned_modules;
    s->owned_modules = node;

    s->runtime_bc_registered_with_jit = true;
    return 0;
}

static int ensure_runtime_and_globals_ready(struct lr_session *s,
                                            session_error_t *err) {
    if (!s || !s->jit || !s->module)
        return 0;

    if (preload_runtime_bc_into_jit(s, err) != 0)
        return -1;

    if (s->module->first_global) {
        if (lr_jit_materialize_globals(s->jit, s->module) != 0) {
            err_set(err, S_ERR_BACKEND, "global materialization failed");
            return -1;
        }
    }
    return 0;
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

static bool module_jit_deferred_until_lookup(const struct lr_session *s) {
    if (!s || !s->jit)
        return false;
    if (s->cfg.mode == SESSION_MODE_IR)
        return true;
    if (s->cfg.mode == SESSION_MODE_DIRECT &&
        s->jit->mode == LR_COMPILE_LLVM)
        return true;
    return false;
}

static int session_backend_to_mode(session_backend_t backend,
                                   lr_compile_mode_t *out_mode) {
    if (!out_mode)
        return -1;
    switch (backend) {
    case SESSION_BACKEND_DEFAULT:
        *out_mode = LR_COMPILE_ISEL;
        return 0;
    case SESSION_BACKEND_ISEL:
        *out_mode = LR_COMPILE_ISEL;
        return 0;
    case SESSION_BACKEND_COPY_PATCH:
        *out_mode = LR_COMPILE_COPY_PATCH;
        return 0;
    case SESSION_BACKEND_LLVM:
        *out_mode = LR_COMPILE_LLVM;
        return 0;
    default:
        return -1;
    }
}

static int ensure_blob_capacity(struct lr_session *s, uint32_t need) {
    if (need <= s->blob_cap)
        return 0;
    uint32_t new_cap = s->blob_cap == 0 ? 8u : s->blob_cap;
    while (new_cap < need)
        new_cap *= 2u;
    lr_func_blob_t *new_blobs = (lr_func_blob_t *)realloc(
        s->blobs, sizeof(*new_blobs) * new_cap);
    if (!new_blobs)
        return -1;
    s->blobs = new_blobs;
    s->blob_cap = new_cap;
    return 0;
}

static int ensure_direct_reloc_range_capacity(struct lr_session *s,
                                              uint32_t need) {
    direct_reloc_range_t *new_ranges;
    uint32_t new_cap;
    if (!s)
        return -1;
    if (need <= s->direct_reloc_range_cap)
        return 0;
    new_cap = s->direct_reloc_range_cap == 0 ? 4u : s->direct_reloc_range_cap;
    while (new_cap < need)
        new_cap *= 2u;
    new_ranges = (direct_reloc_range_t *)realloc(
        s->direct_reloc_ranges, sizeof(*new_ranges) * new_cap);
    if (!new_ranges)
        return -1;
    s->direct_reloc_ranges = new_ranges;
    s->direct_reloc_range_cap = new_cap;
    return 0;
}

static int append_direct_reloc_range(struct lr_session *s,
                                     uint32_t start, uint32_t end) {
    if (!s)
        return -1;
    if (end <= start)
        return 0;
    if (ensure_direct_reloc_range_capacity(s, s->direct_reloc_range_count + 1u) != 0)
        return -1;
    s->direct_reloc_ranges[s->direct_reloc_range_count].start = start;
    s->direct_reloc_ranges[s->direct_reloc_range_count].end = end;
    s->direct_reloc_range_count++;
    return 0;
}

static int close_active_direct_reloc_range(struct lr_session *s) {
    uint32_t end;
    if (!s || !s->direct_reloc_active)
        return 0;
    end = s->direct_obj_ctx_active ? s->direct_obj_ctx.num_relocs : s->direct_reloc_active_start;
    if (append_direct_reloc_range(s, s->direct_reloc_active_start, end) != 0)
        return -1;
    s->direct_reloc_active = false;
    return 0;
}

static uint32_t direct_reloc_range_reloc_count(const struct lr_session *s) {
    uint32_t total = 0;
    if (!s || !s->direct_reloc_ranges)
        return 0;
    for (uint32_t i = 0; i < s->direct_reloc_range_count; i++) {
        const direct_reloc_range_t *rr = &s->direct_reloc_ranges[i];
        if (rr->end > rr->start)
            total += rr->end - rr->start;
    }
    return total;
}

static int init_direct_obj_ctx(struct lr_session *s) {
    if (s->direct_obj_ctx_active)
        return 0;
    memset(&s->direct_obj_ctx, 0, sizeof(s->direct_obj_ctx));
    if (lr_obj_build_symbol_cache(&s->direct_obj_ctx, s->module) != 0)
        return -1;
    s->direct_obj_ctx_active = true;
    return 0;
}

static bool module_has_defined_symbol_linear(const lr_module_t *m,
                                             const char *name) {
    if (!m || !name || !name[0])
        return false;
    for (const lr_func_t *f = m->first_func; f; f = f->next) {
        if (!f->name || strcmp(f->name, name) != 0)
            continue;
        if (f->first_block || !f->is_decl)
            return true;
    }
    for (const lr_global_t *g = m->first_global; g; g = g->next) {
        if (!g->name || strcmp(g->name, name) != 0)
            continue;
        if (!g->is_external)
            return true;
    }
    return false;
}

static bool module_has_symbol_linear(const lr_module_t *m,
                                     const char *name) {
    if (!m || !name || !name[0])
        return false;
    for (const lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return true;
    }
    for (const lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->name && strcmp(g->name, name) == 0)
            return true;
    }
    return false;
}

static bool session_is_module_defined_symbol(struct lr_session *s,
                                             const char *name) {
    if (!s || !s->module || !name || !name[0])
        return false;
    if (module_has_defined_symbol_linear(s->module, name))
        return true;
    /* Forward references are legal in DIRECT mode: defer relocation
       patching while a symbol is known to belong to the current module,
       even if its body is emitted later. */
    if (module_has_symbol_linear(s->module, name))
        return true;
    if (!s->direct_obj_ctx_active)
        return false;
    uint32_t sym_id = lr_module_intern_symbol(s->module, name);
    if (sym_id >= s->direct_obj_ctx.module_sym_count)
        return false;
    if (s->direct_obj_ctx.module_sym_defined &&
        s->direct_obj_ctx.module_sym_defined[sym_id] != 0)
        return true;
    if (s->direct_obj_ctx.module_sym_funcs &&
        s->direct_obj_ctx.module_sym_funcs[sym_id])
        return true;
    return false;
}

/* Returns 0 when patched, 1 when unresolved symbol remains, -1 on hard error. */
static int patch_direct_relocs(struct lr_session *s, uint32_t reloc_start,
                               const char **missing_symbol) {
    const char *missing = NULL;
    bool opened_update = false;
    if (missing_symbol)
        *missing_symbol = NULL;
    if (!s || !s->jit || !s->direct_obj_ctx_active)
        return 0;
    if (!s->jit->update_active) {
        lr_jit_begin_update(s->jit);
        opened_update = s->jit->update_active;
        if (!opened_update)
            return -1;
    }
    if (lr_jit_patch_relocs_from_ex(s->jit, &s->direct_obj_ctx,
                                    reloc_start, &missing) == 0) {
        if (opened_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return 0;
    }
    if (opened_update && s->jit->update_active)
        lr_jit_end_update(s->jit);
    if (missing_symbol)
        *missing_symbol = missing;
    if (missing && missing[0])
        return 1;
    return -1;
}

/* Patch relocations in [range_start, range_end). */
static int patch_direct_reloc_range(struct lr_session *s,
                                    uint32_t range_start,
                                    uint32_t range_end,
                                    const char **missing_symbol) {
    uint32_t saved_num;
    int rc;
    if (!s || range_end <= range_start)
        return 0;
    saved_num = s->direct_obj_ctx.num_relocs;
    s->direct_obj_ctx.num_relocs = range_end;
    rc = patch_direct_relocs(s, range_start, missing_symbol);
    s->direct_obj_ctx.num_relocs = saved_num;
    return rc;
}

static void try_patch_pending_direct_relocs(struct lr_session *s) {
    const char *missing_symbol = NULL;
    int patch_rc;
    if (!s || !s->direct_pending_relocs || !s->direct_obj_ctx_active)
        return;
    patch_rc = patch_direct_relocs(s, s->direct_pending_reloc_start,
                                   &missing_symbol);
    if (patch_rc == 0) {
        s->direct_pending_relocs = false;
        s->direct_pending_reloc_start = 0;
    } else {
        (void)missing_symbol;
    }
}

static int begin_direct_compile(struct lr_session *s, session_error_t *err) {
    if (!s || !s->cur_func)
        return 0;

    if (s->cfg.mode != SESSION_MODE_DIRECT)
        return 0;
    if (s->jit && s->jit->mode == LR_COMPILE_LLVM) {
        if (!lr_llvm_jit_is_available()) {
            err_set(err, S_ERR_MODE,
                    "DIRECT+llvm requires llvm-c LLJIT support");
            return -1;
        }
        s->direct_llvm_stream = true;
        return 0;
    }
    if (!direct_mode_enabled(s)) {
        const char *mode_name = (s && s->jit) ?
            lr_compile_mode_name(s->jit->mode) : "unknown";
        const char *target_name = (s && s->jit && s->jit->target &&
                                   s->jit->target->name) ?
            s->jit->target->name : "unknown";
        err_set(err, S_ERR_MODE,
                "DIRECT policy unsupported for target=%s mode=%s",
                target_name, mode_name ? mode_name : "unknown");
        return -1;
    }

    /* Initialize the obj_ctx for relocation capture on first function */
    if (init_direct_obj_ctx(s) != 0) {
        err_set(err, S_ERR_BACKEND, "obj_ctx initialization failed");
        return -1;
    }

    /* Install obj_ctx so the backend emits relocatable code */
    s->module->obj_ctx = &s->direct_obj_ctx;
    s->direct_reloc_base = s->direct_obj_ctx.num_relocs;
    s->direct_reloc_range_count = 0;
    s->direct_reloc_active_start = s->direct_obj_ctx.num_relocs;
    s->direct_reloc_active = true;

    /* Allocate (or grow) the per-function temp buffer from available JIT
       code capacity so large functions do not hit fixed-size temp limits. */
    {
        size_t desired_cap = direct_compile_buf_capacity(s);
        uint8_t *new_buf;
        if (desired_cap == 0) {
            err_set(err, S_ERR_BACKEND, "no available JIT code capacity");
            s->module->obj_ctx = NULL;
            return -1;
        }
        if (s->func_compile_buf && s->func_compile_buf_cap >= desired_cap) {
            /* Reuse existing allocation when already large enough. */
        } else {
            new_buf = (uint8_t *)realloc(s->func_compile_buf, desired_cap);
            if (!new_buf) {
                err_set(err, S_ERR_BACKEND, "function compile buffer alloc failed");
                s->module->obj_ctx = NULL;
                return -1;
            }
            s->func_compile_buf = new_buf;
            s->func_compile_buf_cap = desired_cap;
        }
    }

    /* Ensure the function symbol exists in the obj_ctx symbol table so the
       backend can emit relocations against it. Mark as undefined here;
       finish_direct_compile sets the real offset once code is placed. */
    if (s->cur_func->name && s->cur_func->name[0]) {
        uint32_t sym_idx = lr_obj_ensure_symbol(
            &s->direct_obj_ctx, s->cur_func->name,
            false, 0, 0);
        if (sym_idx == UINT32_MAX) {
            err_set(err, S_ERR_BACKEND, "function symbol registration failed");
            s->module->obj_ctx = NULL;
            return -1;
        }
    }

    /* Defer backend emission to function end where lr_func_finalize (DCE)
       runs first, matching LLVM's behavior of never generating machine code
       for dead instructions (e.g. loads from null-derived pointers). */
    s->compile_active = true;
    s->compile_deferred = true;
    s->compile_opened_update = false;
    return 0;
}

static int capture_blob(struct lr_session *s, const uint8_t *code,
                        size_t code_len) {
    lr_objfile_ctx_t *oc = &s->direct_obj_ctx;
    uint32_t num_relocs = direct_reloc_range_reloc_count(s);

    if (ensure_blob_capacity(s, s->blob_count + 1u) != 0)
        return -1;

    lr_func_blob_t *blob = &s->blobs[s->blob_count];
    memset(blob, 0, sizeof(*blob));

    blob->name = s->cur_func->name;

    /* Copy pre-relocation code bytes */
    uint8_t *code_copy = (uint8_t *)malloc(code_len > 0 ? code_len : 1u);
    if (!code_copy)
        return -1;
    memcpy(code_copy, code, code_len);
    blob->code = code_copy;
    blob->code_len = code_len;

    /* Convert obj relocs (index-based) to cached relocs (name-based).
       At this point reloc offsets are function-relative (not yet adjusted
       to absolute), so they can be stored directly in the blob. */
    if (num_relocs > 0) {
        lr_cached_reloc_t *cached = (lr_cached_reloc_t *)calloc(
            num_relocs, sizeof(*cached));
        if (!cached) {
            free(code_copy);
            return -1;
        }
        uint32_t ci = 0;
        for (uint32_t rgi = 0; rgi < s->direct_reloc_range_count; rgi++) {
            const direct_reloc_range_t *rr = &s->direct_reloc_ranges[rgi];
            for (uint32_t ri = rr->start; ri < rr->end; ri++) {
                const lr_obj_reloc_t *rel = &oc->relocs[ri];
                if (rel->symbol_idx >= oc->num_symbols) {
                    free(cached);
                    free(code_copy);
                    return -1;
                }
                cached[ci].offset = rel->offset;
                cached[ci].type = rel->type;
                cached[ci].symbol_name = oc->symbols[rel->symbol_idx].name;
                ci++;
            }
        }
        blob->relocs = cached;
        blob->num_relocs = num_relocs;
    }

    s->blob_count++;
    return 0;
}

static const uint8_t k_blob_pkg_magic[8] = {
    'L', 'R', 'B', 'L', 'O', 'B', '1', '\0'
};

static void blob_pkg_w32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)(v);
    (*p)[1] = (uint8_t)(v >> 8);
    (*p)[2] = (uint8_t)(v >> 16);
    (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}

static void blob_pkg_w64(uint8_t **p, uint64_t v) {
    blob_pkg_w32(p, (uint32_t)(v & 0xffffffffu));
    blob_pkg_w32(p, (uint32_t)(v >> 32));
}

static int blob_pkg_r32(const uint8_t **p, const uint8_t *end, uint32_t *out) {
    const uint8_t *q = *p;
    if (!p || !out || !q || q + 4 > end)
        return -1;
    *out = ((uint32_t)q[0]) |
           ((uint32_t)q[1] << 8) |
           ((uint32_t)q[2] << 16) |
           ((uint32_t)q[3] << 24);
    *p = q + 4;
    return 0;
}

static int blob_pkg_r64(const uint8_t **p, const uint8_t *end, uint64_t *out) {
    uint32_t lo = 0, hi = 0;
    if (blob_pkg_r32(p, end, &lo) != 0 ||
        blob_pkg_r32(p, end, &hi) != 0)
        return -1;
    *out = ((uint64_t)hi << 32) | (uint64_t)lo;
    return 0;
}

static int module_intern_name_slice(lr_module_t *m,
                                    const uint8_t *bytes,
                                    uint32_t len,
                                    const char **out_name) {
    char *tmp = NULL;
    uint32_t sym_id;
    const char *interned = NULL;
    if (!m || !bytes || !out_name)
        return -1;
    tmp = (char *)malloc((size_t)len + 1u);
    if (!tmp)
        return -1;
    memcpy(tmp, bytes, len);
    tmp[len] = '\0';
    sym_id = lr_module_intern_symbol(m, tmp);
    free(tmp);
    interned = lr_module_symbol_name(m, sym_id);
    if (!interned)
        return -1;
    *out_name = interned;
    return 0;
}

int lr_session_export_blob_package(struct lr_session *s,
                                   uint8_t **out_data,
                                   size_t *out_len,
                                   session_error_t *err) {
    size_t total = 0;
    uint8_t *buf = NULL;
    uint8_t *p = NULL;

    err_clear(err);
    if (!s || !out_data || !out_len) {
        err_set(err, S_ERR_ARGUMENT, "invalid export_blob_package arguments");
        return -1;
    }
    /* Blob export is valid for both DIRECT and IR sessions.
       IR-mode producers typically export an empty package (no captured blobs),
       but imported blobs are still serialized if present. */

    total = 8 + 4 + 4; /* magic + version + blob_count */
    for (uint32_t bi = 0; bi < s->blob_count; bi++) {
        const lr_func_blob_t *blob = &s->blobs[bi];
        size_t name_len;
        if (!blob->name || !blob->name[0]) {
            err_set(err, S_ERR_STATE, "blob export encountered unnamed function");
            return -1;
        }
        name_len = strlen(blob->name);
        total += 4 + name_len;        /* name */
        total += 8;                   /* code_len */
        total += 4;                   /* num_relocs */
        total += blob->code_len;      /* code bytes */
        for (uint32_t ri = 0; ri < blob->num_relocs; ri++) {
            const char *sym = blob->relocs[ri].symbol_name;
            size_t sym_len;
            if (!sym || !sym[0]) {
                err_set(err, S_ERR_STATE,
                        "blob export encountered relocation without symbol name");
                return -1;
            }
            sym_len = strlen(sym);
            total += 4;               /* offset */
            total += 1 + 3;           /* type + padding */
            total += 4 + sym_len;     /* symbol name */
        }
    }

    buf = (uint8_t *)malloc(total > 0 ? total : 1u);
    if (!buf) {
        err_set(err, S_ERR_BACKEND, "blob package allocation failed");
        return -1;
    }
    p = buf;
    memcpy(p, k_blob_pkg_magic, 8);
    p += 8;
    blob_pkg_w32(&p, 1u); /* version */
    blob_pkg_w32(&p, s->blob_count);

    for (uint32_t bi = 0; bi < s->blob_count; bi++) {
        const lr_func_blob_t *blob = &s->blobs[bi];
        uint32_t name_len = (uint32_t)strlen(blob->name);
        blob_pkg_w32(&p, name_len);
        memcpy(p, blob->name, name_len);
        p += name_len;
        blob_pkg_w64(&p, (uint64_t)blob->code_len);
        blob_pkg_w32(&p, blob->num_relocs);
        if (blob->code_len > 0) {
            memcpy(p, blob->code, blob->code_len);
            p += blob->code_len;
        }
        for (uint32_t ri = 0; ri < blob->num_relocs; ri++) {
            const lr_cached_reloc_t *r = &blob->relocs[ri];
            uint32_t sym_len = (uint32_t)strlen(r->symbol_name);
            blob_pkg_w32(&p, r->offset);
            *p++ = r->type;
            *p++ = 0;
            *p++ = 0;
            *p++ = 0;
            blob_pkg_w32(&p, sym_len);
            memcpy(p, r->symbol_name, sym_len);
            p += sym_len;
        }
    }
    if ((size_t)(p - buf) != total) {
        free(buf);
        err_set(err, S_ERR_BACKEND, "blob package size mismatch");
        return -1;
    }

    *out_data = buf;
    *out_len = total;
    return 0;
}

int lr_session_import_blob_package(struct lr_session *s,
                                   const uint8_t *data,
                                   size_t len,
                                   session_error_t *err) {
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint32_t version = 0;
    uint32_t blob_count = 0;
    uint32_t orig_blob_count = 0;

    err_clear(err);
    if (!s || !data || len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid import_blob_package arguments");
        return -1;
    }
    if (!s->module) {
        err_set(err, S_ERR_STATE, "session has no module for blob import");
        return -1;
    }
    if (len < 16 || memcmp(p, k_blob_pkg_magic, 8) != 0) {
        err_set(err, S_ERR_PARSE, "invalid blob package magic");
        return -1;
    }
    p += 8;
    if (blob_pkg_r32(&p, end, &version) != 0 ||
        blob_pkg_r32(&p, end, &blob_count) != 0) {
        err_set(err, S_ERR_PARSE, "invalid blob package header");
        return -1;
    }
    if (version != 1u) {
        err_set(err, S_ERR_PARSE, "unsupported blob package version");
        return -1;
    }

    orig_blob_count = s->blob_count;
    for (uint32_t bi = 0; bi < blob_count; bi++) {
        uint32_t name_len = 0;
        uint64_t code_len_u64 = 0;
        uint32_t num_relocs = 0;
        const char *interned_name = NULL;
        uint8_t *code_copy = NULL;
        lr_cached_reloc_t *relocs = NULL;
        lr_func_blob_t *blob = NULL;

        if (blob_pkg_r32(&p, end, &name_len) != 0 ||
            name_len == 0 || p + name_len > end) {
            err_set(err, S_ERR_PARSE, "invalid blob function name");
            goto fail;
        }
        if (module_intern_name_slice(s->module, p, name_len, &interned_name) != 0) {
            err_set(err, S_ERR_BACKEND, "failed to intern blob function name");
            goto fail;
        }
        p += name_len;
        if (blob_pkg_r64(&p, end, &code_len_u64) != 0 ||
            code_len_u64 > (uint64_t)(end - p)) {
            err_set(err, S_ERR_PARSE, "invalid blob code payload");
            goto fail;
        }
        if (blob_pkg_r32(&p, end, &num_relocs) != 0) {
            err_set(err, S_ERR_PARSE, "invalid blob relocation header");
            goto fail;
        }
        if (ensure_blob_capacity(s, s->blob_count + 1u) != 0) {
            err_set(err, S_ERR_BACKEND, "blob capacity growth failed");
            goto fail;
        }
        if (code_len_u64 > 0) {
            code_copy = (uint8_t *)malloc((size_t)code_len_u64);
            if (!code_copy) {
                err_set(err, S_ERR_BACKEND, "blob code allocation failed");
                goto fail;
            }
            memcpy(code_copy, p, (size_t)code_len_u64);
        }
        p += (size_t)code_len_u64;
        if (num_relocs > 0) {
            relocs = (lr_cached_reloc_t *)calloc(num_relocs, sizeof(*relocs));
            if (!relocs) {
                err_set(err, S_ERR_BACKEND, "blob relocation allocation failed");
                goto fail;
            }
            for (uint32_t ri = 0; ri < num_relocs; ri++) {
                uint32_t sym_len = 0;
                const char *interned_sym = NULL;
                if (blob_pkg_r32(&p, end, &relocs[ri].offset) != 0 ||
                    p + 1 + 3 > end) {
                    err_set(err, S_ERR_PARSE, "invalid blob relocation entry");
                    goto fail;
                }
                relocs[ri].type = *p++;
                p += 3; /* reserved padding */
                if (blob_pkg_r32(&p, end, &sym_len) != 0 ||
                    sym_len == 0 || p + sym_len > end) {
                    err_set(err, S_ERR_PARSE, "invalid blob relocation symbol");
                    goto fail;
                }
                if (module_intern_name_slice(s->module, p, sym_len, &interned_sym) != 0) {
                    err_set(err, S_ERR_BACKEND, "failed to intern relocation symbol");
                    goto fail;
                }
                relocs[ri].symbol_name = interned_sym;
                p += sym_len;
            }
        }

        blob = &s->blobs[s->blob_count];
        memset(blob, 0, sizeof(*blob));
        blob->name = interned_name;
        blob->code = code_copy;
        blob->code_len = (size_t)code_len_u64;
        blob->relocs = relocs;
        blob->num_relocs = num_relocs;
        s->blob_count++;
    }

    if (p != end) {
        err_set(err, S_ERR_PARSE, "blob package has trailing bytes");
        goto fail;
    }
    return 0;

fail:
    while (s->blob_count > orig_blob_count) {
        lr_func_blob_t *blob = &s->blobs[s->blob_count - 1u];
        free((void *)blob->code);
        free((void *)blob->relocs);
        memset(blob, 0, sizeof(*blob));
        s->blob_count--;
    }
    return -1;
}

static int finish_direct_compile(struct lr_session *s, void **out_addr,
                                 session_error_t *err) {
    size_t code_len = 0;
    int rc;
    bool should_close_update;

    if (!s || !s->cur_func || !s->jit || !s->compile_active) {
        err_set(err, S_ERR_STATE, "no active direct compile context");
        return -1;
    }
    if (!s->compile_deferred && !s->compile_ctx) {
        err_set(err, S_ERR_STATE, "no active direct compile context");
        return -1;
    }

    /* The update may have been opened by begin_direct_compile (for
       streaming backends that emit code in compile_begin). */
    if (!s->jit->update_active) {
        lr_jit_begin_update(s->jit);
        s->compile_opened_update = s->jit->update_active;
    }
    if (!s->jit->update_active) {
        err_set(err, S_ERR_BACKEND, "jit update transition failed");
        s->module->obj_ctx = NULL;
        return -1;
    }
    should_close_update = s->compile_opened_update;
    /* Reassert writable state even if update_active is already true.
       MAP_JIT write protection is thread-local, so this can drift. */
    lr_jit_begin_update(s->jit);
    if (!s->jit->update_active) {
        err_set(err, S_ERR_BACKEND, "jit code buffer not writable");
        s->module->obj_ctx = NULL;
        if (should_close_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }

    /* Deferred compilation: finalize IR (runs DCE to eliminate dead
       instructions like loads from null-derived pointers), then replay
       finalized IR through the backend. This matches LLVM's ISel behavior
       of never generating machine code for unused instruction results. */
    if (s->compile_deferred) {
        lr_compile_func_meta_t meta;
        lr_arena_t *arena = s->module->arena;

        if (lr_func_finalize(s->cur_func, arena) != 0) {
            err_set(err, S_ERR_BACKEND, "function finalization failed");
            s->module->obj_ctx = NULL;
            if (should_close_update && s->jit->update_active)
                lr_jit_end_update(s->jit);
            return -1;
        }

        memset(&meta, 0, sizeof(meta));
        meta.func = s->cur_func;
        meta.ret_type = s->cur_func->ret_type;
        meta.param_types = s->cur_func->param_types;
        meta.num_params = s->cur_func->num_params;
        meta.vararg = s->cur_func->vararg;
        meta.num_blocks = s->cur_func->num_blocks;
        meta.next_vreg = s->cur_func->next_vreg;
        meta.mode = s->jit->mode;
        meta.jit = s->jit;

        rc = s->jit->target->compile_begin(
            &s->compile_ctx, &meta, s->module,
            s->func_compile_buf, s->func_compile_buf_cap,
            arena);
        if (rc != 0 || !s->compile_ctx) {
            err_set(err, S_ERR_BACKEND, "deferred compile_begin failed");
            s->module->obj_ctx = NULL;
            if (should_close_update && s->jit->update_active)
                lr_jit_end_update(s->jit);
            return -1;
        }

        rc = lr_replay_function_stream(s->jit->target, s->compile_ctx,
                                        s->cur_func);
        if (rc != 0) {
            err_set(err, S_ERR_BACKEND, "deferred replay failed");
            s->module->obj_ctx = NULL;
            if (should_close_update && s->jit->update_active)
                lr_jit_end_update(s->jit);
            return -1;
        }

        s->compile_deferred = false;
    }

    rc = s->jit->target->compile_end(s->compile_ctx, &code_len);
    s->compile_ctx = NULL;
    s->compile_active = false;
    s->compile_opened_update = false;
    if (rc != 0) {
        err_set(err, S_ERR_BACKEND, "backend compile end failed");
        s->module->obj_ctx = NULL;
        if (should_close_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }
    if (close_active_direct_reloc_range(s) != 0) {
        err_set(err, S_ERR_BACKEND, "relocation range tracking failed");
        s->module->obj_ctx = NULL;
        if (should_close_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }

    /* Assign final JIT offset now that compile_end has produced the code
       in the per-function temp buffer. */
    s->compile_start = align_up_size(s->jit->code_size, 16u);
    if (s->compile_start + code_len > s->jit->code_cap) {
        err_set(err, S_ERR_BACKEND, "jit code buffer overflow");
        s->module->obj_ctx = NULL;
        if (should_close_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }

    /* Copy compiled code from per-function temp buffer to JIT code buffer */
    if (code_len > 0)
        memcpy(s->jit->code_buf + s->compile_start,
               s->func_compile_buf, code_len);

    /* Define the symbol in obj_ctx with the real JIT position.
       The symbol was registered as undefined in begin_direct_compile. */
    if (s->cur_func->name && s->cur_func->name[0]) {
        uint32_t sym_idx = lr_obj_ensure_symbol(
            &s->direct_obj_ctx, s->cur_func->name,
            true, 1, (uint32_t)s->compile_start);
        (void)sym_idx;
    }

    /* Capture blob (pre-relocation code + relocs) for later exe/obj emission.
       Must happen before adjusting reloc offsets to absolute. */
    if (capture_blob(s, s->jit->code_buf + s->compile_start, code_len) != 0) {
        err_set(err, S_ERR_BACKEND, "blob capture failed");
        s->module->obj_ctx = NULL;
        if (should_close_update && s->jit->update_active)
            lr_jit_end_update(s->jit);
        return -1;
    }

    /* Adjust reloc offsets from function-relative to absolute within the
       JIT code buffer, but only for relocation ranges owned by this function. */
    for (uint32_t rgi = 0; rgi < s->direct_reloc_range_count; rgi++) {
        const direct_reloc_range_t *rr = &s->direct_reloc_ranges[rgi];
        for (uint32_t ri = rr->start; ri < rr->end; ri++)
            s->direct_obj_ctx.relocs[ri].offset += (uint32_t)s->compile_start;
    }

    /* Apply JIT relocations on the live code copy for immediate execution,
       restricted to the current function's relocation ranges. */
    s->jit->code_size = s->compile_start + code_len;
    if (s->jit->update_active && code_len > 0)
        s->jit->update_dirty = true;
    {
        int patch_rc = 0;
        const char *missing_symbol = NULL;
        for (uint32_t rgi = 0; rgi < s->direct_reloc_range_count; rgi++) {
            const direct_reloc_range_t *rr = &s->direct_reloc_ranges[rgi];
            const char *range_missing = NULL;
            int rc2 = patch_direct_reloc_range(s, rr->start, rr->end,
                                               &range_missing);
            if (rc2 < 0) {
                patch_rc = rc2;
                break;
            }
            if (rc2 > 0) {
                if (patch_rc == 0) {
                    patch_rc = rc2;
                    missing_symbol = range_missing;
                }
                /* In DIRECT mode, unresolved relocations are deferred until
                   lookup/execution time so forward references and late-bound
                   externals do not fail function emission. */
                if (!s->direct_pending_relocs ||
                    rr->start < s->direct_pending_reloc_start) {
                    s->direct_pending_reloc_start = rr->start;
                }
                s->direct_pending_relocs = true;
            }
        }
        if (patch_rc < 0) {
            err_set(err, S_ERR_BACKEND, "direct relocation patching failed");
            s->module->obj_ctx = NULL;
            if (should_close_update && s->jit->update_active)
                lr_jit_end_update(s->jit);
            return -1;
        }
        (void)missing_symbol;
    }

    lr_jit_add_symbol(s->jit, s->cur_func->name,
                      s->jit->code_buf + s->compile_start);
    s->cur_func->is_decl = true;

    /* Update symbol cache so subsequent functions know this one is defined */
    if (s->direct_obj_ctx_active) {
        uint32_t sym_id = lr_module_intern_symbol(s->module, s->cur_func->name);
        if (sym_id < s->direct_obj_ctx.module_sym_count)
            s->direct_obj_ctx.module_sym_defined[sym_id] = 1;
    }

    if (s->direct_pending_relocs) {
        const char *missing_symbol = NULL;
        int patch_rc = patch_direct_relocs(s, s->direct_pending_reloc_start,
                                           &missing_symbol);
        if (patch_rc == 0) {
            s->direct_pending_relocs = false;
            s->direct_pending_reloc_start = 0;
        } else if (patch_rc < 0) {
            err_set(err, S_ERR_BACKEND, "direct relocation patching failed");
            s->module->obj_ctx = NULL;
            if (should_close_update && s->jit->update_active)
                lr_jit_end_update(s->jit);
            return -1;
        } else {
            (void)missing_symbol;
        }
    }

    s->module->obj_ctx = NULL;

    if (out_addr)
        *out_addr = s->jit->code_buf + s->compile_start;

    if (should_close_update && s->jit->update_active)
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
    lr_block_t *b;
    bool has_blocks = false;
    if (!s || !s->cur_func)
        return -1;
    if (!s->cur_func->first_block) {
        err_set(err, S_ERR_STATE, "block 0 is not terminated");
        return -1;
    }

    for (b = s->cur_func->first_block; b; b = b->next) {
        uint32_t id = b->id;
        bool terminated = false;
        has_blocks = true;

        if (ensure_block_capacity(s, id + 1u) != 0) {
            err_set(err, S_ERR_BACKEND, "block table allocation failed");
            return -1;
        }
        if (!s->blocks[id])
            s->blocks[id] = b;
        if (s->block_count <= id)
            s->block_count = id + 1u;

        terminated = s->block_seen[id] && s->block_terminated[id];
        if (!terminated && b->last)
            terminated = is_terminator(b->last->op);

        if (!terminated) {
            lr_inst_t *term = lr_inst_create(s->module->arena,
                                             LR_OP_UNREACHABLE,
                                             NULL, 0, NULL, 0);
            lr_compile_inst_desc_t term_desc;
            if (!term) {
                err_set(err, S_ERR_BACKEND,
                        "failed to synthesize terminator for block %u", id);
                return -1;
            }
            if (s->compile_active && s->cur_block == b) {
                if (!s->compile_ctx || !s->jit || !s->jit->target ||
                    !s->jit->target->compile_emit) {
                    err_set(err, S_ERR_STATE,
                            "no active direct compile context for synthesized terminator");
                    return -1;
                }
                memset(&term_desc, 0, sizeof(term_desc));
                term_desc.op = LR_OP_UNREACHABLE;
                term_desc.type = s->module->type_void;
                if (s->jit->target->compile_emit(s->compile_ctx, &term_desc) != 0) {
                    err_set(err, S_ERR_BACKEND,
                            "backend emit failed for synthesized terminator in block %u",
                            id);
                    return -1;
                }
            }
            lr_block_append(b, term);
            terminated = true;
        }

        s->block_seen[id] = true;
        s->block_terminated[id] = terminated;
    }
    if (!has_blocks) {
        err_set(err, S_ERR_STATE, "block 0 is not terminated");
        return -1;
    }
    return 0;
}

static void finish_function_state(struct lr_session *s) {
    if (!s)
        return;
    if (s->compile_opened_update && s->jit && s->jit->update_active)
        lr_jit_end_update(s->jit);
    if (s->module)
        s->module->obj_ctx = NULL;
    reset_block_tracking(s);
    reset_phi_copies(s);
    s->cur_func = NULL;
    s->cur_block = NULL;
    s->block_count = 0;
    s->compile_ctx = NULL;
    s->compile_start = 0;
    s->compile_active = false;
    s->compile_deferred = false;
    s->direct_llvm_stream = false;
    s->compile_opened_update = false;
    s->emitted_count = 0;
    s->direct_reloc_range_count = 0;
    s->direct_reloc_active = false;
    if (s->null_derived)
        memset(s->null_derived, 0, s->null_derived_cap);
}

/* ---- Suspend / resume for interleaved function generation -------------- */

static int ensure_suspended_capacity(struct lr_session *s, uint32_t need) {
    suspended_compile_t *new_list;
    uint32_t new_cap;
    if (need <= s->suspended_cap)
        return 0;
    new_cap = s->suspended_cap == 0 ? 4u : s->suspended_cap;
    while (new_cap < need)
        new_cap *= 2u;
    new_list = (suspended_compile_t *)realloc(
        s->suspended, sizeof(*new_list) * new_cap);
    if (!new_list)
        return -1;
    s->suspended = new_list;
    s->suspended_cap = new_cap;
    return 0;
}

int lr_session_suspend_func(struct lr_session *s) {
    suspended_compile_t *slot;
    if (!s || !s->cur_func || !s->compile_active)
        return -1;
    if (ensure_suspended_capacity(s, s->suspended_count + 1u) != 0)
        return -1;
    if (close_active_direct_reloc_range(s) != 0)
        return -1;

    slot = &s->suspended[s->suspended_count];
    memset(slot, 0, sizeof(*slot));
    slot->func = s->cur_func;
    slot->cur_block = s->cur_block;
    slot->blocks = s->blocks;
    slot->block_seen = s->block_seen;
    slot->block_terminated = s->block_terminated;
    slot->block_count = s->block_count;
    slot->block_cap = s->block_cap;
    slot->phi_copies = s->phi_copies;
    slot->phi_copy_count = s->phi_copy_count;
    slot->phi_copy_cap = s->phi_copy_cap;
    slot->compile_ctx = s->compile_ctx;
    slot->direct_reloc_base = s->direct_reloc_base;
    slot->direct_reloc_ranges = s->direct_reloc_ranges;
    slot->direct_reloc_range_count = s->direct_reloc_range_count;
    slot->direct_reloc_range_cap = s->direct_reloc_range_cap;
    slot->compile_active = s->compile_active;
    slot->compile_deferred = s->compile_deferred;
    slot->compile_opened_update = s->compile_opened_update;
    slot->emitted_count = s->emitted_count;
    slot->null_derived = s->null_derived;
    slot->null_derived_cap = s->null_derived_cap;

    /* Move the per-function temp buffer ownership to the suspended slot.
       A new buffer will be allocated when the next function begins. */
    slot->func_buf = s->func_compile_buf;
    slot->func_buf_cap = s->func_compile_buf_cap;
    s->func_compile_buf = NULL;
    s->func_compile_buf_cap = 0;

    s->suspended_count++;

    /* Close the JIT update that was opened for this function's compile. */
    if (s->compile_opened_update && s->jit && s->jit->update_active)
        lr_jit_end_update(s->jit);

    /* Clear session state without freeing the arrays (now owned by slot) */
    s->cur_func = NULL;
    s->cur_block = NULL;
    s->blocks = NULL;
    s->block_seen = NULL;
    s->block_terminated = NULL;
    s->block_count = 0;
    s->block_cap = 0;
    s->phi_copies = NULL;
    s->phi_copy_count = 0;
    s->phi_copy_cap = 0;
    s->compile_ctx = NULL;
    s->compile_start = 0;
    s->compile_active = false;
    s->compile_deferred = false;
    s->compile_opened_update = false;
    s->emitted_count = 0;
    s->null_derived = NULL;
    s->null_derived_cap = 0;
    s->direct_reloc_ranges = NULL;
    s->direct_reloc_range_count = 0;
    s->direct_reloc_range_cap = 0;
    s->direct_reloc_active = false;
    if (s->module)
        s->module->obj_ctx = NULL;

    return 0;
}

int lr_session_resume_func(struct lr_session *s, uint32_t suspended_idx) {
    suspended_compile_t *slot;
    if (!s || suspended_idx >= s->suspended_count)
        return -1;
    if (s->cur_func)
        return -1;

    slot = &s->suspended[suspended_idx];

    /* Free any existing block tracking arrays (should be NULL after
       suspend, but guard against leaks). */
    free(s->blocks);
    free(s->block_seen);
    free(s->block_terminated);
    free(s->phi_copies);
    free(s->direct_reloc_ranges);
    free(s->null_derived);

    /* Restore all compile state from the suspended slot */
    s->cur_func = slot->func;
    s->cur_block = slot->cur_block;
    s->blocks = slot->blocks;
    s->block_seen = slot->block_seen;
    s->block_terminated = slot->block_terminated;
    s->block_count = slot->block_count;
    s->block_cap = slot->block_cap;
    s->phi_copies = slot->phi_copies;
    s->phi_copy_count = slot->phi_copy_count;
    s->phi_copy_cap = slot->phi_copy_cap;
    s->compile_ctx = slot->compile_ctx;
    s->direct_reloc_base = slot->direct_reloc_base;
    s->direct_reloc_ranges = slot->direct_reloc_ranges;
    s->direct_reloc_range_count = slot->direct_reloc_range_count;
    s->direct_reloc_range_cap = slot->direct_reloc_range_cap;
    s->compile_active = slot->compile_active;
    s->compile_deferred = slot->compile_deferred;
    s->compile_opened_update = slot->compile_opened_update;
    s->emitted_count = slot->emitted_count;
    s->null_derived = slot->null_derived;
    s->null_derived_cap = slot->null_derived_cap;
    s->direct_reloc_active_start = s->direct_obj_ctx_active ?
        s->direct_obj_ctx.num_relocs : s->direct_reloc_base;
    s->direct_reloc_active = true;

    /* Restore the per-function temp buffer */
    free(s->func_compile_buf);
    s->func_compile_buf = slot->func_buf;
    s->func_compile_buf_cap = slot->func_buf_cap;

    /* Re-open JIT update for the resumed compile context.
       In deferred mode, JIT update is opened later at compile time. */
    if (s->compile_active && s->jit) {
        if (!s->compile_deferred && !s->jit->update_active) {
            lr_jit_begin_update(s->jit);
            s->compile_opened_update = s->jit->update_active;
        }
        s->module->obj_ctx = &s->direct_obj_ctx;
    }

    /* Remove from suspended list by shifting remaining entries down */
    s->suspended_count--;
    if (suspended_idx < s->suspended_count) {
        memmove(&s->suspended[suspended_idx],
                &s->suspended[suspended_idx + 1],
                sizeof(*s->suspended) * (s->suspended_count - suspended_idx));
    }

    return 0;
}

int lr_session_find_suspended(struct lr_session *s, lr_func_t *func) {
    uint32_t i;
    if (!s)
        return -1;
    if (!func) {
        /* Return first suspended entry index, or -1 if none */
        return s->suspended_count > 0 ? 0 : -1;
    }
    for (i = 0; i < s->suspended_count; i++) {
        if (s->suspended[i].func == func)
            return (int)i;
    }
    return -1;
}

static void free_suspended_list(struct lr_session *s) {
    uint32_t i;
    if (!s)
        return;
    for (i = 0; i < s->suspended_count; i++) {
        free(s->suspended[i].blocks);
        free(s->suspended[i].block_seen);
        free(s->suspended[i].block_terminated);
        free(s->suspended[i].phi_copies);
        free(s->suspended[i].direct_reloc_ranges);
        free(s->suspended[i].func_buf);
        free(s->suspended[i].null_derived);
    }
    free(s->suspended);
    s->suspended = NULL;
    s->suspended_count = 0;
    s->suspended_cap = 0;
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

    /* IR and DIRECT+llvm without address request: finalize only and defer
       JIT until lookup() when the full module is available. */
    if (module_jit_deferred_until_lookup(s) && !out_addr)
        return 0;

    if (s->cfg.mode == SESSION_MODE_IR ||
        (s->cfg.mode == SESSION_MODE_DIRECT &&
         s->jit && s->jit->mode == LR_COMPILE_LLVM))
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
    lr_compile_mode_t mode = LR_COMPILE_ISEL;
    err_clear(err);

    if (cfg) {
        if (cfg->mode != SESSION_MODE_DIRECT &&
            cfg->mode != SESSION_MODE_IR) {
            err_set(err, S_ERR_ARGUMENT, "invalid session mode");
            return NULL;
        }
        if (session_backend_to_mode(cfg->backend, &mode) != 0) {
            err_set(err, S_ERR_ARGUMENT, "invalid session backend");
            return NULL;
        }
    } else {
        mode = LR_COMPILE_ISEL;
    }

    s = (struct lr_session *)calloc(1, sizeof(*s));
    if (!s) {
        err_set(err, S_ERR_BACKEND, "session allocation failed");
        return NULL;
    }

    if (cfg) {
        s->cfg.mode = cfg->mode;
        s->cfg.target = cfg->target;
        s->cfg.backend = cfg->backend;
    }

    arena = lr_arena_create(0);
    if (!arena) {
        free(s);
        err_set(err, S_ERR_BACKEND, "arena allocation failed");
        return NULL;
    }

    s->owned_module = lr_module_create(arena);
    s->module = s->owned_module;
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
        lr_module_free(s->owned_module);
        free(s);
        err_set(err, S_ERR_BACKEND, "jit creation failed");
        return NULL;
    }
    s->jit->mode = mode;

    return s;
}

void lr_session_replace_jit(struct lr_session *s, lr_jit_t *jit,
                            bool borrowed) {
    if (!s)
        return;
    if (s->jit && s->jit != jit && !s->jit_borrowed)
        lr_jit_destroy(s->jit);
    s->jit = jit;
    s->jit_borrowed = borrowed;
}

void lr_session_destroy(struct lr_session *s) {
    lr_owned_module_t *it = NULL;
    if (!s)
        return;
    if (s->jit && !s->jit_borrowed)
        lr_jit_destroy(s->jit);
    if (s->owned_module)
        lr_module_free(s->owned_module);
    it = s->owned_modules;
    while (it) {
        lr_owned_module_t *next = it->next;
        lr_module_free(it->module);
        free(it);
        it = next;
    }
    for (uint32_t i = 0; i < s->blob_count; i++) {
        free((void *)s->blobs[i].code);
        free((void *)s->blobs[i].relocs);
    }
    free(s->blobs);
    free(s->direct_reloc_ranges);
    if (s->direct_obj_ctx_active)
        lr_objfile_ctx_destroy(&s->direct_obj_ctx);
    free_suspended_list(s);
    free(s->func_compile_buf);
    free(s->phi_copies);
    free(s->block_terminated);
    free(s->block_seen);
    free(s->blocks);
    free(s->null_derived);
    if (!s->runtime_bc_borrowed)
        free(s->runtime_bc_data);
    free(s);
}

/* ---- Symbols ----------------------------------------------------------- */

void lr_session_add_symbol(struct lr_session *s, const char *name, void *addr) {
    if (!s || !s->jit || !name || !name[0])
        return;
    lr_jit_add_symbol(s->jit, name, addr);
    try_patch_pending_direct_relocs(s);
}

int lr_session_load_library(struct lr_session *s, const char *path,
                            session_error_t *err) {
    err_clear(err);
    if (!s || !s->jit || !path || !path[0]) {
        err_set(err, S_ERR_ARGUMENT, "invalid load_library arguments");
        return -1;
    }
    if (lr_jit_load_library(s->jit, path) != 0) {
        err_set(err, S_ERR_BACKEND, "failed to dlopen: %s", path);
        return -1;
    }
    try_patch_pending_direct_relocs(s);
    return 0;
}

static int session_set_runtime_bc_impl(struct lr_session *s,
                                       const uint8_t *bc_data, size_t bc_len,
                                       bool borrowed, session_error_t *err) {
    err_clear(err);
    if (!s || !s->jit || !bc_data || bc_len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid runtime bc arguments");
        return -1;
    }
    if (s->runtime_bc_data) {
        err_set(err, S_ERR_STATE, "runtime bc already configured");
        return -1;
    }
    if (borrowed) {
        s->runtime_bc_data = (uint8_t *)bc_data;
        s->runtime_bc_borrowed = true;
    } else {
        uint8_t *owned = (uint8_t *)malloc(bc_len);
        if (!owned) {
            err_set(err, S_ERR_BACKEND, "runtime bc allocation failed");
            return -1;
        }
        memcpy(owned, bc_data, bc_len);
        s->runtime_bc_data = owned;
        s->runtime_bc_borrowed = false;
    }
    s->runtime_bc_len = bc_len;
    s->runtime_bc_registered_with_jit = false;
    s->runtime_bc_merged_into_main_module = false;

    if (lr_jit_set_runtime_bc_borrowed(s->jit, bc_data, bc_len, borrowed)
        != 0) {
        if (!s->runtime_bc_borrowed)
            free(s->runtime_bc_data);
        s->runtime_bc_data = NULL;
        s->runtime_bc_len = 0;
        s->runtime_bc_borrowed = false;
        err_set(err, S_ERR_BACKEND, "jit runtime bc configuration failed");
        return -1;
    }

    if (preload_runtime_bc_into_jit(s, err) != 0)
        return -1;
    return 0;
}

int lr_session_set_runtime_bc(struct lr_session *s, const uint8_t *bc_data,
                              size_t bc_len, session_error_t *err) {
    return session_set_runtime_bc_impl(s, bc_data, bc_len, false, err);
}

int lr_session_set_runtime_bc_borrowed(struct lr_session *s,
                                       const uint8_t *bc_data, size_t bc_len,
                                       session_error_t *err) {
    return session_set_runtime_bc_impl(s, bc_data, bc_len, true, err);
}

void lr_session_set_runtime_bc_preloaded(struct lr_session *s,
                                         const uint8_t *bc_data,
                                         size_t bc_len) {
    if (!s || !bc_data || bc_len == 0)
        return;
    if (s->runtime_bc_data)
        return;
    s->runtime_bc_data = (uint8_t *)bc_data;
    s->runtime_bc_len = bc_len;
    s->runtime_bc_borrowed = true;
    s->runtime_bc_registered_with_jit = true;
}

void *lr_session_lookup(struct lr_session *s, const char *name) {
    void *addr;
    if (!s || !s->jit || !name || !name[0])
        return NULL;
    if (preload_runtime_bc_into_jit(s, NULL) != 0)
        return NULL;
    if (module_jit_deferred_until_lookup(s) && !s->ir_module_jit_ready) {
        if (lr_jit_add_module(s->jit, s->module) != 0)
            return NULL;
        s->ir_module_jit_ready = true;
    }
    if (s->module && s->module->first_global) {
        if (lr_jit_materialize_globals(s->jit, s->module) != 0)
            return NULL;
    }
    if (s->direct_pending_relocs && s->direct_obj_ctx_active) {
        const char *missing_symbol = NULL;
        int patch_rc = patch_direct_relocs(s, s->direct_pending_reloc_start,
                                           &missing_symbol);
        if (patch_rc == 0) {
            s->direct_pending_relocs = false;
            s->direct_pending_reloc_start = 0;
        } else if (patch_rc < 0) {
            return NULL;
        } else if (!session_is_module_defined_symbol(s, missing_symbol)) {
            return NULL;
        }
        if (s->direct_pending_relocs)
            return NULL;
    }
    addr = lr_jit_get_function(s->jit, name);
    return addr;
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

lr_type_t *lr_type_vector_s(struct lr_session *s, lr_type_t *elem,
                             uint64_t count) {
    if (!s || !s->module || !elem)
        return NULL;
    return lr_type_vector(s->module->arena, elem, count);
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
    s->ir_module_jit_ready = false;
    if (s->jit && lr_jit_materialize_globals(s->jit, s->module) == 0)
        try_patch_pending_direct_relocs(s);
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
    s->ir_module_jit_ready = false;
    if (s->jit && lr_jit_materialize_globals(s->jit, s->module) == 0)
        try_patch_pending_direct_relocs(s);
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
    s->ir_module_jit_ready = false;
    if (s->jit && lr_jit_materialize_globals(s->jit, s->module) == 0)
        try_patch_pending_direct_relocs(s);
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
    s->ir_module_jit_ready = false;
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
    s->direct_llvm_stream = false;
    s->emitted_count = 0;
    s->ir_module_jit_ready = false;
    if (ensure_runtime_and_globals_ready(s, err) != 0) {
        s->cur_func->is_decl = true;
        finish_function_state(s);
        return -1;
    }
    if (begin_direct_compile(s, err) != 0) {
        s->cur_func->is_decl = true;
        finish_function_state(s);
        return -1;
    }
    return 0;
}

int lr_session_func_begin_existing(struct lr_session *s, lr_module_t *module,
                                    lr_func_t *func, session_error_t *err) {
    err_clear(err);
    if (!s || !module || !func) {
        err_set(err, S_ERR_ARGUMENT,
                "invalid func_begin_existing arguments");
        return -1;
    }
    if (s->cur_func) {
        err_set(err, S_ERR_STATE, "function already active");
        return -1;
    }

    s->module = module;
    s->cur_func = func;
    s->cur_block = NULL;
    s->block_count = 0;
    reset_phi_copies(s);
    s->compile_ctx = NULL;
    s->compile_start = 0;
    s->compile_active = false;
    s->direct_llvm_stream = false;
    s->emitted_count = 0;
    s->ir_module_jit_ready = false;

    if (ensure_runtime_and_globals_ready(s, err) != 0) {
        finish_function_state(s);
        return -1;
    }

    if (begin_direct_compile(s, err) != 0) {
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
                            uint32_t succ_block_id,
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
    if (s->compile_active && !s->compile_deferred &&
        s->compile_ctx && s->jit &&
        s->jit->target && s->jit->target->compile_add_phi_copy) {
        if (s->jit->target->compile_add_phi_copy(
                s->compile_ctx, pred_block_id, succ_block_id,
                copy->dest_vreg, &copy->src_op) != 0) {
            err_set(err, S_ERR_BACKEND, "backend phi copy failed");
            return -1;
        }
    }
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

    /* When JIT is deferred (IR mode, DIRECT+llvm) and no address is
       requested, skip validation and compilation entirely.  The compat
       layer may switch between functions, so running
       validate_block_termination here would add spurious "unreachable"
       terminators to blocks that are still being constructed.  Likewise
       lr_func_finalize would DCE instructions whose users haven't been
       emitted yet.  Both steps happen later when the full module is
       available: LLVM via its own verifier/optimizer after serialization,
       isel/copy_patch via lr_target_compile. */
    if (!s->compile_active &&
        module_jit_deferred_until_lookup(s) && !out_addr) {
        finish_function_state(s);
        return 0;
    }

    if (validate_block_termination(s, err) != 0)
        return -1;

    if (s->compile_active)
        rc = finish_direct_compile(s, out_addr, err);
    else
        rc = compile_current_function(s, out_addr, err);
    if (rc != 0) {
        finish_function_state(s);
        return -1;
    }

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
    if (s->compile_active && !s->compile_deferred) {
        if (!s->compile_ctx || !s->jit->target ||
            !s->jit->target->compile_set_block ||
            s->jit->target->compile_set_block(s->compile_ctx, block_id) != 0) {
            err_set(err, S_ERR_BACKEND, "backend set-block failed");
            return -1;
        }
    }
    return 0;
}

int lr_session_adopt_block(struct lr_session *s, uint32_t block_id,
                           lr_block_t *block, session_error_t *err) {
    err_clear(err);
    if (!s || !s->cur_func || !block) {
        err_set(err, S_ERR_ARGUMENT, "invalid adopt_block arguments");
        return -1;
    }
    if (ensure_block_capacity(s, block_id + 1u) != 0) {
        err_set(err, S_ERR_BACKEND, "block table allocation failed");
        return -1;
    }
    if (s->block_count <= block_id)
        s->block_count = block_id + 1u;
    s->blocks[block_id] = block;
    s->cur_block = block;
    if (s->compile_active && !s->compile_deferred) {
        if (!s->compile_ctx || !s->jit->target ||
            !s->jit->target->compile_set_block ||
            s->jit->target->compile_set_block(s->compile_ctx, block_id) != 0) {
            err_set(err, S_ERR_BACKEND, "backend set-block failed");
            return -1;
        }
    }
    return 0;
}

int lr_session_bind_ir(struct lr_session *s, lr_module_t *module,
                       lr_func_t *func, lr_block_t *block,
                       session_error_t *err) {
    err_clear(err);
    if (!s || !module || !func || !block || block->func != func) {
        err_set(err, S_ERR_ARGUMENT, "invalid bind arguments");
        return -1;
    }
    s->module = module;
    if (ensure_block_capacity(s, block->id + 1u) != 0) {
        err_set(err, S_ERR_BACKEND, "block table allocation failed");
        return -1;
    }
    if (s->block_count <= block->id)
        s->block_count = block->id + 1u;
    s->blocks[block->id] = block;
    s->cur_func = func;
    s->cur_block = block;
    s->compile_active = false;
    s->direct_llvm_stream = false;
    s->compile_ctx = NULL;
    s->ir_module_jit_ready = false;
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
        normalized.operands[0].kind == LR_OP_KIND_GLOBAL &&
        !s->module->obj_ctx) {
        /* Pre-resolve call targets when NOT in relocatable mode.
           When obj_ctx is active, the backend emits relocations for
           GLOBAL operands instead, which get patched by JIT relocs
           after compile_end and stay name-based in captured blobs. */
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
        } else {
            err_set(err, S_ERR_BACKEND,
                    "direct call target unresolved: %s",
                    callee_name ? callee_name : "(unknown)");
            return 0;
        }
    }

    if (s->compile_active && !s->compile_deferred) {
        bool skip_backend = false;

        if (!s->compile_ctx || !s->jit->target || !s->jit->target->compile_emit) {
            err_set(err, S_ERR_STATE, "no active direct compile context");
            free(resolved_call_ops);
            return 0;
        }

        /* Track null-derived vregs so we can skip loads that would
           dereference null and crash.  LLVM's ISel silently drops such
           dead loads; the streaming path must do the same. */
        if (normalized.op == LR_OP_GEP &&
            normalized.num_operands >= 1 &&
            operand_is_null_derived(s, &normalized.operands[0]))
            null_derived_mark(s, dest);
        else if ((normalized.op == LR_OP_BITCAST ||
                  normalized.op == LR_OP_INTTOPTR) &&
                 normalized.num_operands >= 1 &&
                 operand_is_null_derived(s, &normalized.operands[0]))
            null_derived_mark(s, dest);
        else if (normalized.op == LR_OP_LOAD &&
                 normalized.num_operands >= 1 &&
                 operand_is_null_derived(s, &normalized.operands[0]))
            skip_backend = true;

        if (!skip_backend) {
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
            if (s->jit->target->compile_emit(s->compile_ctx,
                                              &compile_desc) != 0) {
                err_set(err, S_ERR_BACKEND, "backend emit failed for op %d",
                        (int)normalized.op);
                free(resolved_call_ops);
                return 0;
            }
        }
        /* Extract phi copies from PHI operand pairs and forward to backend. */
        if (normalized.op == LR_OP_PHI &&
            s->jit->target->compile_add_phi_copy &&
            normalized.num_operands >= 2) {
            for (uint32_t pi = 0; pi + 1 < normalized.num_operands; pi += 2) {
                uint32_t pred_id = 0;
                if (normalized.operands[pi + 1].kind == LR_OP_KIND_BLOCK)
                    pred_id = normalized.operands[pi + 1].block_id;
                if (s->jit->target->compile_add_phi_copy(
                        s->compile_ctx, pred_id, s->cur_block->id, dest,
                        &normalized.operands[pi]) != 0) {
                    err_set(err, S_ERR_BACKEND, "backend phi copy failed");
                    free(resolved_call_ops);
                    return 0;
                }
            }
        }
        /*
         * Keep the IR module in sync even in DIRECT mode so textual dumps
         * (for example --show-llvm in WITH_LIRIC lanes) preserve instruction
         * semantics instead of CFG-only skeletons.
         */
        if (emit_ir_instruction(s, &normalized, err, NULL) != 0) {
            free(resolved_call_ops);
            return 0;
        }
    } else {
        /* IR-only emit: covers both deferred DIRECT mode and IR mode.
           In deferred DIRECT, lr_func_finalize (DCE) runs at function end
           before backend compilation, matching LLVM's ISel behavior. */
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

int lr_session_set_module(struct lr_session *s, lr_module_t *module,
                          session_error_t *err) {
    err_clear(err);
    if (!s || !module) {
        err_set(err, S_ERR_ARGUMENT, "invalid set_module arguments");
        return -1;
    }
    if (s->compile_active || s->direct_llvm_stream || s->cur_func) {
        err_set(err, S_ERR_STATE, "cannot switch module during active function");
        return -1;
    }
    s->module = module;
    s->ir_module_jit_ready = false;
    return 0;
}

static int session_compile_parsed_module(struct lr_session *s, lr_module_t *m,
                                         const char *input_kind,
                                         void **out_addr,
                                         session_error_t *err) {
    lr_func_t *last_def = NULL;
    int rc;

    if (!s || !s->jit || !m) {
        err_set(err, S_ERR_ARGUMENT, "invalid compiled module arguments");
        return -1;
    }

    if (preload_runtime_bc_into_jit(s, err) != 0) {
        lr_module_free(m);
        return -1;
    }

    rc = lr_jit_add_module(s->jit, m);
    if (rc != 0) {
        lr_module_free(m);
        err_set(err, S_ERR_BACKEND, "%s module code generation failed",
                input_kind ? input_kind : "input");
        return -1;
    }

    if (register_owned_module(s, m, err) != 0) {
        lr_module_free(m);
        return -1;
    }

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

static const lr_target_t *session_resolve_target(struct lr_session *s) {
    if (s->cfg.target && s->cfg.target[0])
        return lr_target_by_name(s->cfg.target);
    return lr_target_host();
}

int lr_session_emit_object(struct lr_session *s, const char *path,
                            session_error_t *err) {
    char backend_err[256] = {0};

    err_clear(err);
    if (!s || !s->module || !path) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_object arguments");
        return -1;
    }
    if (s->blob_count > 0) {
        const lr_target_t *target = session_resolve_target(s);
        if (!target) {
            err_set(err, S_ERR_BACKEND, "target not found");
            return -1;
        }
        FILE *out = fopen(path, "wb");
        if (!out) {
            err_set(err, S_ERR_BACKEND, "cannot open output: %s", path);
            return -1;
        }
        int rc = lr_emit_object_from_blobs(s->blobs, s->blob_count,
                                           s->module, target, out);
        (void)fclose(out);
        if (rc != 0) {
            err_set(err, S_ERR_BACKEND, "blob object emission failed");
            return -1;
        }
        return 0;
    }

    if (lr_emit_module_object_path_mode(s->module, s->cfg.target,
                                        s->jit ? s->jit->mode : LR_COMPILE_ISEL,
                                        path, backend_err,
                                        sizeof(backend_err)) != 0) {
        err_set(err, S_ERR_BACKEND, "%s",
                backend_err[0] ? backend_err : "object emission failed");
        return -1;
    }
    return 0;
}

int lr_session_emit_object_stream(struct lr_session *s, FILE *out,
                                  session_error_t *err) {
    err_clear(err);
    if (!s || !s->module || !out) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_object_stream arguments");
        return -1;
    }
    if (s->blob_count > 0) {
        const lr_target_t *target = session_resolve_target(s);
        if (!target) {
            err_set(err, S_ERR_BACKEND, "target not found");
            return -1;
        }
        return lr_emit_object_from_blobs(s->blobs, s->blob_count,
                                         s->module, target, out);
    }

    /* IR mode: compile from IR using session's compile mode */
    lr_compile_mode_t mode = s->jit ? s->jit->mode : LR_COMPILE_ISEL;
    char backend_err[256] = {0};
    const lr_target_t *target = session_resolve_target(s);
    if (!target) {
        err_set(err, S_ERR_BACKEND, "target not found");
        return -1;
    }

    if (mode == LR_COMPILE_LLVM) {
#if defined(__unix__) || defined(__APPLE__)
        char tmp_tpl[] = "/tmp/liric_emit_obj_XXXXXX";
        int fd = mkstemp(tmp_tpl);
        int rc = -1;
        if (fd < 0) {
            err_set(err, S_ERR_BACKEND, "temporary file creation failed");
            return -1;
        }
        close(fd);
        rc = lr_llvm_emit_object_path(s->module, target, tmp_tpl,
                                      backend_err, sizeof(backend_err));
        if (rc == 0) {
            FILE *in = fopen(tmp_tpl, "rb");
            if (in) {
                uint8_t buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                    if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
                }
                if (ferror(in)) rc = -1;
                fclose(in);
            } else {
                rc = -1;
            }
        }
        unlink(tmp_tpl);
        if (rc != 0) {
            err_set(err, S_ERR_BACKEND, "llvm object stream emission failed: %s",
                    backend_err[0] ? backend_err : "copy failed");
            return -1;
        }
        return 0;
#else
        err_set(err, S_ERR_BACKEND, "llvm object stream emission unsupported");
        return -1;
#endif
    }

    if (lr_emit_object(s->module, target, out) != 0) {
        err_set(err, S_ERR_BACKEND, "object emission failed");
        return -1;
    }
    return 0;
}

int lr_session_emit_exe(struct lr_session *s, const char *path,
                         session_error_t *err) {
    char backend_err[256] = {0};
    const char *entry = NULL;

    err_clear(err);
    if (!s || !s->module || !path) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_exe arguments");
        return -1;
    }
    if (s->runtime_bc_data && s->runtime_bc_len > 0) {
        if (merge_runtime_bc_into_module(s, s->module,
                                         &s->runtime_bc_merged_into_main_module,
                                         err) != 0)
            return -1;
    }
    if (s->blob_count > 0) {
        const lr_target_t *target = session_resolve_target(s);
        if (!target) {
            err_set(err, S_ERR_BACKEND, "target not found");
            return -1;
        }
        FILE *out = fopen(path, "wb");
        if (!out) {
            err_set(err, S_ERR_BACKEND, "cannot open output: %s", path);
            return -1;
        }
        entry = session_blob_entry_symbol(s, session_entry_symbol(s->module));
        int rc = lr_emit_executable_from_blobs(s->blobs, s->blob_count,
                                               s->module, target, out,
                                               entry);
        (void)fclose(out);
        if (rc != 0) {
            err_set(err, S_ERR_BACKEND, "blob executable emission failed");
            return -1;
        }
        return 0;
    }

    entry = session_entry_symbol(s->module);
    if (lr_emit_module_executable_path_mode(
            s->module, s->cfg.target,
            s->jit ? s->jit->mode : LR_COMPILE_ISEL,
            path, entry, NULL, 0,
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
    bool has_runtime_bc;
    const char *entry = NULL;

    err_clear(err);
    if (!s || !s->module || !path) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_exe_with_runtime arguments");
        return -1;
    }
    has_runtime_bc = s->runtime_bc_data && s->runtime_bc_len > 0;
    if (has_runtime_bc) {
        return lr_session_emit_exe(s, path, err);
    }
    if (!runtime_ll || runtime_len == 0) {
        err_set(err, S_ERR_ARGUMENT, "invalid emit_exe_with_runtime arguments");
        return -1;
    }

    if (s->blob_count > 0) {
        char parse_err[256] = {0};
        lr_module_t *parsed_runtime = NULL;

        parsed_runtime = lr_parse_ll(runtime_ll, runtime_len, parse_err, sizeof(parse_err));
        if (!parsed_runtime) {
            err_set(err, S_ERR_PARSE, "runtime ll parse failed: %s",
                    parse_err[0] ? parse_err : "unknown parse error");
            return -1;
        }
        if (lr_module_merge(s->module, parsed_runtime) != 0) {
            lr_module_free(parsed_runtime);
            err_set(err, S_ERR_BACKEND, "runtime ll merge failed");
            return -1;
        }
        lr_module_free(parsed_runtime);

        const lr_target_t *target = session_resolve_target(s);
        if (!target) {
            err_set(err, S_ERR_BACKEND, "target not found");
            return -1;
        }
        FILE *out = fopen(path, "wb");
        if (!out) {
            err_set(err, S_ERR_BACKEND, "cannot open output: %s", path);
            return -1;
        }
        entry = session_blob_entry_symbol(s, session_entry_symbol(s->module));
        int rc = lr_emit_executable_from_blobs(s->blobs, s->blob_count,
                                               s->module, target, out,
                                               entry);
        (void)fclose(out);
        if (rc != 0) {
            err_set(err, S_ERR_BACKEND, "blob executable emission failed");
            return -1;
        }
        return 0;
    }

    entry = session_entry_symbol(s->module);
    if (lr_emit_module_executable_path_mode(
            s->module, s->cfg.target,
            s->jit ? s->jit->mode : LR_COMPILE_ISEL,
            path, entry, runtime_ll, runtime_len,
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

bool lr_session_is_direct(struct lr_session *s) {
    return s && s->cfg.mode == SESSION_MODE_DIRECT;
}

bool lr_session_is_compiling(struct lr_session *s) {
    return s && (s->compile_active || s->direct_llvm_stream);
}

lr_func_t *lr_session_cur_func(struct lr_session *s) {
    return s ? s->cur_func : NULL;
}

lr_block_t *lr_session_cur_block(struct lr_session *s) {
    return s ? s->cur_block : NULL;
}

lr_jit_t *lr_session_jit(struct lr_session *s) {
    return s ? s->jit : NULL;
}
