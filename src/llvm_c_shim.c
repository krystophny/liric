#include "jit.h"
#include "platform/platform_os.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lc_module_compat lc_module_compat_t;
int lc_module_finalize_for_execution(lc_module_compat_t *mod);
int lc_module_add_to_jit(lc_module_compat_t *mod, lr_jit_t *jit);
lr_module_t *lc_module_get_ir(lc_module_compat_t *mod);

typedef struct LLVMLiricSessionState LLVMLiricSessionState;
typedef LLVMLiricSessionState *LLVMLiricSessionStateRef;

typedef enum liric_lookup_ret_kind {
    LIRIC_LOOKUP_RET_OTHER = 0,
    LIRIC_LOOKUP_RET_F32,
    LIRIC_LOOKUP_RET_F64,
    LIRIC_LOOKUP_RET_C32,
    LIRIC_LOOKUP_RET_C64,
} liric_lookup_ret_kind_t;

typedef struct liric_lookup_sig_entry {
    char *name;
    uint32_t num_params;
    liric_lookup_ret_kind_t ret_kind;
    struct liric_lookup_sig_entry *next;
} liric_lookup_sig_entry_t;

typedef struct liric_lookup_wrapper_entry {
    char *name;
    void *target;
    liric_lookup_ret_kind_t ret_kind;
    void *code;
    size_t code_len;
    struct liric_lookup_wrapper_entry *next;
} liric_lookup_wrapper_entry_t;

typedef struct liric_operand_view {
    int kind;
    lr_type_t *type;
    int64_t global_offset;
    union {
        uint32_t vreg;
        int64_t imm_i64;
        double imm_f64;
        uint32_t block_id;
        uint32_t global_id;
    };
} liric_operand_view_t;

struct LLVMLiricSessionState {
    lr_jit_t *jit;
    liric_lookup_sig_entry_t *lookup_sigs;
    liric_lookup_wrapper_entry_t *lookup_wrappers;
};

static int read_file_bytes(const char *path, uint8_t **out_data, size_t *out_len) {
    FILE *f = NULL;
    long len = 0;
    uint8_t *buf = NULL;
    size_t nread = 0;
    if (!path || !path[0] || !out_data || !out_len)
        return -1;
    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    len = ftell(f);
    if (len <= 0) {
        fclose(f);
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buf = (uint8_t *)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return -1;
    }
    nread = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (nread != (size_t)len) {
        free(buf);
        return -1;
    }
    *out_data = buf;
    *out_len = nread;
    return 0;
}

static bool is_two_lane_fp_aggregate(const lr_type_t *type,
                                     lr_type_kind_t lane_kind) {
    if (!type)
        return false;
    if (type->kind == LR_TYPE_STRUCT) {
        if (type->struc.num_fields != 2 || !type->struc.fields)
            return false;
        return type->struc.fields[0] &&
               type->struc.fields[1] &&
               type->struc.fields[0]->kind == lane_kind &&
               type->struc.fields[1]->kind == lane_kind;
    }
    if (type->kind == LR_TYPE_ARRAY || type->kind == LR_TYPE_VECTOR) {
        return type->array.count == 2 &&
               type->array.elem &&
               type->array.elem->kind == lane_kind;
    }
    return false;
}

static liric_lookup_ret_kind_t classify_lookup_ret_kind(const lr_type_t *ret_type) {
    if (!ret_type)
        return LIRIC_LOOKUP_RET_OTHER;
    if (ret_type->kind == LR_TYPE_FLOAT)
        return LIRIC_LOOKUP_RET_F32;
    if (ret_type->kind == LR_TYPE_DOUBLE)
        return LIRIC_LOOKUP_RET_F64;
    if (is_two_lane_fp_aggregate(ret_type, LR_TYPE_FLOAT))
        return LIRIC_LOOKUP_RET_C32;
    if (is_two_lane_fp_aggregate(ret_type, LR_TYPE_DOUBLE))
        return LIRIC_LOOKUP_RET_C64;
    return LIRIC_LOOKUP_RET_OTHER;
}

static liric_lookup_sig_entry_t *find_lookup_sig_entry(
    LLVMLiricSessionStateRef state, const char *name) {
    liric_lookup_sig_entry_t *entry;
    if (!state || !name || !name[0])
        return NULL;
    for (entry = state->lookup_sigs; entry; entry = entry->next) {
        if (entry->name && strcmp(entry->name, name) == 0)
            return entry;
    }
    return NULL;
}

static void upsert_lookup_sig_entry(LLVMLiricSessionStateRef state,
                                    const char *name, uint32_t num_params,
                                    liric_lookup_ret_kind_t ret_kind) {
    liric_lookup_sig_entry_t *entry;
    if (!state || !name || !name[0])
        return;
    entry = find_lookup_sig_entry(state, name);
    if (!entry) {
        entry = (liric_lookup_sig_entry_t *)calloc(1, sizeof(*entry));
        if (!entry)
            return;
        entry->name = strdup(name);
        if (!entry->name) {
            free(entry);
            return;
        }
        entry->next = state->lookup_sigs;
        state->lookup_sigs = entry;
    }
    entry->num_params = num_params;
    entry->ret_kind = ret_kind;
}

static void record_module_lookup_signatures(LLVMLiricSessionStateRef state,
                                            const lr_module_t *module) {
    const lr_func_t *f;
    if (!state || !module)
        return;
    for (f = module->first_func; f; f = f->next) {
        if (!f->name || !f->name[0] || f->is_decl)
            continue;
        upsert_lookup_sig_entry(state, f->name, f->num_params,
                                classify_lookup_ret_kind(f->ret_type));
    }
}

static bool module_declares_symbol(const lr_module_t *module, const char *name) {
    const lr_func_t *f;
    const lr_global_t *g;
    if (!module || !name || !name[0])
        return false;
    for (f = module->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return true;
    }
    for (g = module->first_global; g; g = g->next) {
        if (g->name && strcmp(g->name, name) == 0)
            return true;
    }
    return false;
}

static int validate_module_data_global_refs(const lr_module_t *module) {
    const lr_func_t *f;
    if (!module)
        return -1;
    for (f = module->first_func; f; f = f->next) {
        const lr_block_t *b;
        if (f->is_decl)
            continue;
        for (b = f->first_block; b; b = b->next) {
            const lr_inst_t *inst;
            for (inst = b->first; inst; inst = inst->next) {
                for (uint32_t i = 0; i < inst->num_operands; i++) {
                    const liric_operand_view_t *ops =
                        (const liric_operand_view_t *)inst->operands;
                    const liric_operand_view_t *op = &ops[i];
                    const char *sym_name;
                    if (op->kind != LR_VAL_GLOBAL)
                        continue;
                    if (inst->op == LR_OP_CALL && i == 0)
                        continue;
                    sym_name = lr_module_symbol_name(module, op->global_id);
                    if (!sym_name || !sym_name[0])
                        continue;
                    if (module_declares_symbol(module, sym_name))
                        continue;
                    return -1;
                }
            }
        }
    }
    return 0;
}

static liric_lookup_wrapper_entry_t *find_lookup_wrapper(
    LLVMLiricSessionStateRef state, const char *name, void *target,
    liric_lookup_ret_kind_t ret_kind) {
    liric_lookup_wrapper_entry_t *entry;
    if (!state || !name || !name[0] || !target)
        return NULL;
    for (entry = state->lookup_wrappers; entry; entry = entry->next) {
        if (entry->target == target &&
            entry->ret_kind == ret_kind &&
            entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool emit_u8(uint8_t *buf, size_t cap, size_t *pos, uint8_t byte) {
    if (!buf || !pos || *pos >= cap)
        return false;
    buf[*pos] = byte;
    (*pos)++;
    return true;
}

static bool emit_u64_le(uint8_t *buf, size_t cap, size_t *pos, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        if (!emit_u8(buf, cap, pos, (uint8_t)(v & 0xFFu)))
            return false;
        v >>= 8;
    }
    return true;
}

static bool emit_sub_rsp_imm8(uint8_t *buf, size_t cap, size_t *pos, uint8_t imm) {
    return emit_u8(buf, cap, pos, 0x48) &&
           emit_u8(buf, cap, pos, 0x83) &&
           emit_u8(buf, cap, pos, 0xEC) &&
           emit_u8(buf, cap, pos, imm);
}

static bool emit_add_rsp_imm8(uint8_t *buf, size_t cap, size_t *pos, uint8_t imm) {
    return emit_u8(buf, cap, pos, 0x48) &&
           emit_u8(buf, cap, pos, 0x83) &&
           emit_u8(buf, cap, pos, 0xC4) &&
           emit_u8(buf, cap, pos, imm);
}

static bool emit_movabs_r11(uint8_t *buf, size_t cap, size_t *pos, uintptr_t addr) {
    return emit_u8(buf, cap, pos, 0x49) &&
           emit_u8(buf, cap, pos, 0xBB) &&
           emit_u64_le(buf, cap, pos, (uint64_t)addr);
}

static bool emit_call_r11(uint8_t *buf, size_t cap, size_t *pos) {
    return emit_u8(buf, cap, pos, 0x41) &&
           emit_u8(buf, cap, pos, 0xFF) &&
           emit_u8(buf, cap, pos, 0xD3);
}

static bool emit_ret(uint8_t *buf, size_t cap, size_t *pos) {
    return emit_u8(buf, cap, pos, 0xC3);
}

static bool emit_movd_xmm0_eax(uint8_t *buf, size_t cap, size_t *pos) {
    return emit_u8(buf, cap, pos, 0x66) &&
           emit_u8(buf, cap, pos, 0x0F) &&
           emit_u8(buf, cap, pos, 0x6E) &&
           emit_u8(buf, cap, pos, 0xC0);
}

static bool emit_movq_xmm0_rax(uint8_t *buf, size_t cap, size_t *pos) {
    return emit_u8(buf, cap, pos, 0x66) &&
           emit_u8(buf, cap, pos, 0x48) &&
           emit_u8(buf, cap, pos, 0x0F) &&
           emit_u8(buf, cap, pos, 0x6E) &&
           emit_u8(buf, cap, pos, 0xC0);
}

static bool emit_lea_rdi_rsp_plus8(uint8_t *buf, size_t cap, size_t *pos) {
    return emit_u8(buf, cap, pos, 0x48) &&
           emit_u8(buf, cap, pos, 0x8D) &&
           emit_u8(buf, cap, pos, 0x7C) &&
           emit_u8(buf, cap, pos, 0x24) &&
           emit_u8(buf, cap, pos, 0x08);
}

static bool emit_movsd_xmm_rsp_offset(uint8_t *buf, size_t cap, size_t *pos,
                                      uint8_t xmm, uint8_t off) {
    uint8_t modrm = (xmm == 0) ? 0x44 : 0x4C;
    return emit_u8(buf, cap, pos, 0xF2) &&
           emit_u8(buf, cap, pos, 0x0F) &&
           emit_u8(buf, cap, pos, 0x10) &&
           emit_u8(buf, cap, pos, modrm) &&
           emit_u8(buf, cap, pos, 0x24) &&
           emit_u8(buf, cap, pos, off);
}

static int build_lookup_wrapper_code(liric_lookup_ret_kind_t ret_kind, void *target,
                                     void **out_code, size_t *out_len) {
#if defined(__x86_64__) || defined(_M_X64)
    bool map_jit_enabled = false;
    uint8_t *code = NULL;
    size_t pos = 0;
    size_t cap = 64;
    bool ok = false;

    if (!target || !out_code || !out_len)
        return -1;

    code = (uint8_t *)lr_platform_alloc_jit_code(cap, &map_jit_enabled);
    if (!code)
        return -1;

    switch (ret_kind) {
    case LIRIC_LOOKUP_RET_F32:
        ok = emit_sub_rsp_imm8(code, cap, &pos, 8) &&
             emit_movabs_r11(code, cap, &pos, (uintptr_t)target) &&
             emit_call_r11(code, cap, &pos) &&
             emit_add_rsp_imm8(code, cap, &pos, 8) &&
             emit_movd_xmm0_eax(code, cap, &pos) &&
             emit_ret(code, cap, &pos);
        break;
    case LIRIC_LOOKUP_RET_F64:
    case LIRIC_LOOKUP_RET_C32:
        ok = emit_sub_rsp_imm8(code, cap, &pos, 8) &&
             emit_movabs_r11(code, cap, &pos, (uintptr_t)target) &&
             emit_call_r11(code, cap, &pos) &&
             emit_add_rsp_imm8(code, cap, &pos, 8) &&
             emit_movq_xmm0_rax(code, cap, &pos) &&
             emit_ret(code, cap, &pos);
        break;
    case LIRIC_LOOKUP_RET_C64:
        ok = emit_sub_rsp_imm8(code, cap, &pos, 24) &&
             emit_lea_rdi_rsp_plus8(code, cap, &pos) &&
             emit_movabs_r11(code, cap, &pos, (uintptr_t)target) &&
             emit_call_r11(code, cap, &pos) &&
             emit_movsd_xmm_rsp_offset(code, cap, &pos, 0, 8) &&
             emit_movsd_xmm_rsp_offset(code, cap, &pos, 1, 16) &&
             emit_add_rsp_imm8(code, cap, &pos, 24) &&
             emit_ret(code, cap, &pos);
        break;
    default:
        break;
    }

    if (!ok || lr_platform_jit_make_executable(code, cap, map_jit_enabled,
                                               NULL, NULL) != 0) {
        (void)lr_platform_free_pages(code, cap);
        return -1;
    }

    *out_code = code;
    *out_len = cap;
    return 0;
#else
    (void)ret_kind;
    (void)target;
    (void)out_code;
    (void)out_len;
    return -1;
#endif
}

LLVMLiricSessionStateRef LLVMLiricSessionCreate(void) {
    const char *runtime_bc = getenv("LIRIC_RUNTIME_BC");
    const char *runtime_lib = getenv("LIRIC_RUNTIME_LIB");
    LLVMLiricSessionStateRef state =
        (LLVMLiricSessionStateRef)calloc(1, sizeof(LLVMLiricSessionState));
    if (!state)
        return NULL;
    state->jit = lr_jit_create();
    if (!state->jit) {
        free(state);
        return NULL;
    }
    if (runtime_bc && runtime_bc[0]) {
        uint8_t *bc = NULL;
        size_t bc_len = 0;
        int rc = read_file_bytes(runtime_bc, &bc, &bc_len);
        if (rc != 0 || lr_jit_set_runtime_bc(state->jit, bc, bc_len) != 0) {
            free(bc);
            lr_jit_destroy(state->jit);
            free(state);
            return NULL;
        }
        free(bc);
    } else if (runtime_lib && runtime_lib[0]) {
        if (lr_jit_load_library(state->jit, runtime_lib) != 0) {
            lr_jit_destroy(state->jit);
            free(state);
            return NULL;
        }
    }
    return state;
}

void LLVMLiricSessionDispose(LLVMLiricSessionStateRef state) {
    if (!state)
        return;
    while (state->lookup_wrappers) {
        liric_lookup_wrapper_entry_t *entry = state->lookup_wrappers;
        state->lookup_wrappers = entry->next;
        if (entry->code && entry->code_len > 0)
            (void)lr_platform_free_pages(entry->code, entry->code_len);
        free(entry->name);
        free(entry);
    }
    while (state->lookup_sigs) {
        liric_lookup_sig_entry_t *entry = state->lookup_sigs;
        state->lookup_sigs = entry->next;
        free(entry->name);
        free(entry);
    }
    if (state->jit)
        lr_jit_destroy(state->jit);
    free(state);
}

int LLVMLiricSessionAddCompatModule(LLVMLiricSessionStateRef state,
                                    lc_module_compat_t *mod) {
    lr_module_t *ir = NULL;
    if (!state || !mod)
        return -1;
    if (!state->jit)
        return -1;
    ir = lc_module_get_ir(mod);
    if (!ir)
        return -1;
    record_module_lookup_signatures(state, ir);
    if (validate_module_data_global_refs(ir) != 0)
        return -1;
    if (lc_module_finalize_for_execution(mod) != 0)
        return -1;
    return lc_module_add_to_jit(mod, state->jit);
}

void LLVMLiricSessionAddSymbol(LLVMLiricSessionStateRef state,
                               const char *name, void *addr) {
    if (!state || !state->jit || !name || !name[0])
        return;
    lr_jit_add_symbol(state->jit, name, addr);
}

int LLVMLiricSessionLoadLibrary(LLVMLiricSessionStateRef state,
                                const char *path) {
    if (!state || !state->jit || !path || !path[0])
        return -1;
    return lr_jit_load_library(state->jit, path);
}

void *LLVMLiricSessionLookup(LLVMLiricSessionStateRef state, const char *name) {
    liric_lookup_sig_entry_t *sig;
    liric_lookup_wrapper_entry_t *wrapper;
    void *addr;
    if (!state || !state->jit || !name || !name[0])
        return NULL;
    addr = lr_jit_get_function(state->jit, name);
    if (!addr)
        return NULL;

    sig = find_lookup_sig_entry(state, name);
    if (!sig || sig->num_params != 0 || sig->ret_kind == LIRIC_LOOKUP_RET_OTHER)
        return addr;

    wrapper = find_lookup_wrapper(state, name, addr, sig->ret_kind);
    if (wrapper)
        return wrapper->code;

    wrapper = (liric_lookup_wrapper_entry_t *)calloc(1, sizeof(*wrapper));
    if (!wrapper)
        return addr;
    wrapper->name = strdup(name);
    if (!wrapper->name) {
        free(wrapper);
        return addr;
    }
    wrapper->target = addr;
    wrapper->ret_kind = sig->ret_kind;
    if (build_lookup_wrapper_code(sig->ret_kind, addr,
                                  &wrapper->code, &wrapper->code_len) != 0) {
        free(wrapper->name);
        free(wrapper);
        return addr;
    }
    wrapper->next = state->lookup_wrappers;
    state->lookup_wrappers = wrapper;
    return wrapper->code;
}

const char *LLVMLiricHostTargetName(void) {
    return lr_jit_host_target_name();
}
