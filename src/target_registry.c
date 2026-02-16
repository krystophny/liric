#include "target.h"
#include <string.h>
#include <stdlib.h>

typedef struct lr_target_entry {
    const char *name;
    const lr_target_t *(*get_target)(void);
} lr_target_entry_t;

static const lr_target_entry_t g_targets[] = {
    { "x86_64", lr_target_x86_64 },
    { "aarch64", lr_target_aarch64 },
    { "arm64", lr_target_aarch64 },
    { "riscv64", lr_target_riscv64 },
    { "riscv", lr_target_riscv64 },
    { "riscv64gc", lr_target_riscv64gc },
    { "rv64gc", lr_target_riscv64gc },
    { "riscv64im", lr_target_riscv64im },
    { "rv64im", lr_target_riscv64im },
};

const lr_target_t *lr_target_by_name(const char *name) {
    if (!name || !name[0])
        return NULL;

    size_t n = sizeof(g_targets) / sizeof(g_targets[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(g_targets[i].name, name) == 0)
            return g_targets[i].get_target();
    }
    return NULL;
}

const lr_target_t *lr_target_host(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return lr_target_by_name("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return lr_target_by_name("aarch64");
#elif defined(__riscv) && __riscv_xlen == 64
#if defined(__riscv_flen) && (__riscv_flen >= 64)
    return lr_target_by_name("riscv64gc");
#else
    return lr_target_by_name("riscv64im");
#endif
#else
    return NULL;
#endif
}

bool lr_target_is_host_compatible(const lr_target_t *t) {
    const lr_target_t *host = lr_target_host();
    return host && t && strcmp(host->name, t->name) == 0;
}

bool lr_target_can_compile(const lr_target_t *target, lr_compile_mode_t mode) {
    if (!target || !target->compile_begin || !target->compile_emit ||
        !target->compile_set_block || !target->compile_end)
        return false;
    if (mode != LR_COMPILE_ISEL &&
        mode != LR_COMPILE_COPY_PATCH)
        return false;
    return true;
}

static int operand_to_desc(const lr_operand_t *op, lr_operand_desc_t *out) {
    if (!op || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    out->type = op->type;
    out->global_offset = op->global_offset;
    switch (op->kind) {
    case LR_VAL_VREG:
        out->kind = LR_OP_KIND_VREG;
        out->vreg = op->vreg;
        return 0;
    case LR_VAL_IMM_I64:
        out->kind = LR_OP_KIND_IMM_I64;
        out->imm_i64 = op->imm_i64;
        return 0;
    case LR_VAL_IMM_F64:
        out->kind = LR_OP_KIND_IMM_F64;
        out->imm_f64 = op->imm_f64;
        return 0;
    case LR_VAL_BLOCK:
        out->kind = LR_OP_KIND_BLOCK;
        out->block_id = op->block_id;
        return 0;
    case LR_VAL_GLOBAL:
        out->kind = LR_OP_KIND_GLOBAL;
        out->global_id = op->global_id;
        return 0;
    case LR_VAL_NULL:
        out->kind = LR_OP_KIND_NULL;
        return 0;
    case LR_VAL_UNDEF:
        out->kind = LR_OP_KIND_UNDEF;
        return 0;
    default:
        return -1;
    }
}

static int replay_phi_copies(const lr_target_t *target, void *compile_ctx,
                             const lr_func_t *func) {
    if (!target->compile_add_phi_copy)
        return 0;
    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        lr_block_t *b = func->block_array[bi];
        if (!b)
            return -1;
        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            if (!inst || inst->op != LR_OP_PHI)
                continue;
            for (uint32_t pi = 0; pi + 1 < inst->num_operands; pi += 2) {
                lr_operand_desc_t val_desc;
                if (operand_to_desc(&inst->operands[pi], &val_desc) != 0)
                    return -1;
                uint32_t pred_id = inst->operands[pi + 1].block_id;
                if (target->compile_add_phi_copy(
                        compile_ctx, pred_id, inst->dest,
                        &val_desc) != 0)
                    return -1;
            }
        }
    }
    return 0;
}

static int replay_function_stream(const lr_target_t *target, void *compile_ctx,
                                  const lr_func_t *func) {
    uint32_t max_operands = 0;
    uint32_t max_indices = 0;
    lr_operand_desc_t *operands = NULL;
    uint32_t *indices = NULL;

    if (!target || !compile_ctx || !func ||
        !target->compile_set_block || !target->compile_emit)
        return -1;

    if (replay_phi_copies(target, compile_ctx, func) != 0)
        return -1;

    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        lr_block_t *b = func->block_array[bi];
        if (!b)
            return -1;
        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            if (!inst)
                return -1;
            if (inst->num_operands > max_operands)
                max_operands = inst->num_operands;
            if (inst->num_indices > max_indices)
                max_indices = inst->num_indices;
        }
    }

    if (max_operands > 0) {
        operands = (lr_operand_desc_t *)calloc(max_operands, sizeof(*operands));
        if (!operands)
            return -1;
    }
    if (max_indices > 0) {
        indices = (uint32_t *)calloc(max_indices, sizeof(*indices));
        if (!indices) {
            free(operands);
            return -1;
        }
    }

    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        lr_block_t *b = func->block_array[bi];
        if (target->compile_set_block(compile_ctx, b->id) != 0) {
            free(indices);
            free(operands);
            return -1;
        }
        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            lr_compile_inst_desc_t desc;
            memset(&desc, 0, sizeof(desc));
            desc.op = inst->op;
            desc.type = inst->type;
            desc.dest = inst->dest;
            desc.num_operands = inst->num_operands;
            desc.num_indices = inst->num_indices;
            desc.icmp_pred = (int)inst->icmp_pred;
            desc.fcmp_pred = (int)inst->fcmp_pred;
            desc.call_external_abi = inst->call_external_abi;
            desc.call_vararg = inst->call_vararg;
            desc.call_fixed_args = inst->call_fixed_args;

            if (inst->num_operands > 0) {
                if (!inst->operands) {
                    free(indices);
                    free(operands);
                    return -1;
                }
                for (uint32_t oi = 0; oi < inst->num_operands; oi++) {
                    if (operand_to_desc(&inst->operands[oi], &operands[oi]) != 0) {
                        free(indices);
                        free(operands);
                        return -1;
                    }
                }
                desc.operands = operands;
            }
            if (inst->num_indices > 0) {
                if (!inst->indices) {
                    free(indices);
                    free(operands);
                    return -1;
                }
                memcpy(indices, inst->indices, inst->num_indices * sizeof(*indices));
                desc.indices = indices;
            }

            if (target->compile_emit(compile_ctx, &desc) != 0) {
                free(indices);
                free(operands);
                return -1;
            }
        }
    }

    free(indices);
    free(operands);
    return 0;
}

int lr_target_compile(const lr_target_t *target, lr_compile_mode_t mode,
                      lr_func_t *func, lr_module_t *mod,
                      uint8_t *buf, size_t buflen, size_t *out_len,
                      lr_arena_t *arena) {
    lr_compile_func_meta_t meta;
    void *compile_ctx = NULL;
    int rc;
    lr_arena_t *layout_arena = (mod && mod->arena) ? mod->arena : arena;

    if (!target || !func || !mod || !buf || !out_len || !arena)
        return -1;
    if (!lr_target_can_compile(target, mode))
        return -1;
    if (!lr_func_is_finalized(func) && lr_func_finalize(func, layout_arena) != 0)
        return -1;

    memset(&meta, 0, sizeof(meta));
    meta.func = func;
    meta.ret_type = func->ret_type;
    meta.param_types = func->param_types;
    meta.num_params = func->num_params;
    meta.vararg = func->vararg;
    meta.num_blocks = func->num_blocks;
    meta.next_vreg = func->next_vreg;
    meta.mode = mode;

    rc = target->compile_begin(&compile_ctx, &meta, mod, buf, buflen, arena);
    if (rc != 0 || !compile_ctx)
        return rc != 0 ? rc : -1;

    rc = replay_function_stream(target, compile_ctx, func);
    if (rc != 0)
        return rc;

    return target->compile_end(compile_ctx, out_len);
}
