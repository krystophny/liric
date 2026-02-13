#include "ir.h"
#include <limits.h>
#include <string.h>

static uint32_t symbol_hash(const char *name) {
    uint32_t h = 2166136261u;
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    return h;
}

static int symbol_index_rebuild(lr_module_t *m, uint32_t min_symbols) {
    uint32_t cap = 128;
    uint32_t i;

    while (cap < (min_symbols << 1))
        cap <<= 1;

    m->symbol_index = lr_arena_array(m->arena, uint32_t, cap);
    if (!m->symbol_index)
        return -1;
    m->symbol_index_cap = cap;

    for (i = 0; i < m->num_symbols; i++) {
        uint32_t slot = m->symbol_hashes[i] & (cap - 1u);
        while (m->symbol_index[slot] != 0)
            slot = (slot + 1u) & (cap - 1u);
        m->symbol_index[slot] = i + 1u;
    }

    return 0;
}

lr_module_t *lr_module_create(lr_arena_t *arena) {
    lr_module_t *m = lr_arena_new(arena, lr_module_t);
    m->arena = arena;

    m->type_void = lr_arena_new(arena, lr_type_t);
    m->type_void->kind = LR_TYPE_VOID;

    m->type_i1 = lr_arena_new(arena, lr_type_t);
    m->type_i1->kind = LR_TYPE_I1;

    m->type_i8 = lr_arena_new(arena, lr_type_t);
    m->type_i8->kind = LR_TYPE_I8;

    m->type_i16 = lr_arena_new(arena, lr_type_t);
    m->type_i16->kind = LR_TYPE_I16;

    m->type_i32 = lr_arena_new(arena, lr_type_t);
    m->type_i32->kind = LR_TYPE_I32;

    m->type_i64 = lr_arena_new(arena, lr_type_t);
    m->type_i64->kind = LR_TYPE_I64;

    m->type_float = lr_arena_new(arena, lr_type_t);
    m->type_float->kind = LR_TYPE_FLOAT;

    m->type_double = lr_arena_new(arena, lr_type_t);
    m->type_double->kind = LR_TYPE_DOUBLE;

    m->type_ptr = lr_arena_new(arena, lr_type_t);
    m->type_ptr->kind = LR_TYPE_PTR;

    return m;
}

lr_type_t *lr_type_func(lr_arena_t *a, lr_type_t *ret, lr_type_t **params,
                         uint32_t num_params, bool vararg) {
    lr_type_t *t = lr_arena_new(a, lr_type_t);
    t->kind = LR_TYPE_FUNC;
    t->func.ret = ret;
    if (num_params > 0) {
        t->func.params = lr_arena_array(a, lr_type_t *, num_params);
        memcpy(t->func.params, params, sizeof(lr_type_t *) * num_params);
    }
    t->func.num_params = num_params;
    t->func.vararg = vararg;
    return t;
}

lr_type_t *lr_type_array(lr_arena_t *a, lr_type_t *elem, uint64_t count) {
    lr_type_t *t = lr_arena_new(a, lr_type_t);
    t->kind = LR_TYPE_ARRAY;
    t->array.elem = elem;
    t->array.count = count;
    return t;
}

lr_type_t *lr_type_struct(lr_arena_t *a, lr_type_t **fields, uint32_t n,
                           bool packed, char *name) {
    lr_type_t *t = lr_arena_new(a, lr_type_t);
    t->kind = LR_TYPE_STRUCT;
    if (n > 0) {
        t->struc.fields = lr_arena_array(a, lr_type_t *, n);
        memcpy(t->struc.fields, fields, sizeof(lr_type_t *) * n);
    }
    t->struc.num_fields = n;
    t->struc.packed = packed;
    t->struc.name = name;
    return t;
}

lr_func_t *lr_func_create(lr_module_t *m, const char *name, lr_type_t *ret,
                           lr_type_t **params, uint32_t num_params, bool vararg) {
    lr_arena_t *a = m->arena;
    lr_func_t *f = lr_arena_new(a, lr_func_t);
    f->name = lr_arena_strdup(a, name, strlen(name));
    f->ret_type = ret;
    f->type = lr_type_func(a, ret, params, num_params, vararg);
    f->num_params = num_params;
    f->vararg = vararg;
    if (num_params > 0) {
        f->param_types = lr_arena_array(a, lr_type_t *, num_params);
        memcpy(f->param_types, params, sizeof(lr_type_t *) * num_params);
        f->param_vregs = lr_arena_array(a, uint32_t, num_params);
        for (uint32_t i = 0; i < num_params; i++)
            f->param_vregs[i] = f->next_vreg++;
    }
    if (!m->first_func) m->first_func = f;
    else m->last_func->next = f;
    m->last_func = f;
    return f;
}

lr_func_t *lr_func_declare(lr_module_t *m, const char *name, lr_type_t *ret,
                            lr_type_t **params, uint32_t num_params, bool vararg) {
    lr_func_t *f = lr_func_create(m, name, ret, params, num_params, vararg);
    f->is_decl = true;
    return f;
}

lr_block_t *lr_block_create(lr_func_t *f, lr_arena_t *a, const char *name) {
    lr_block_t *b = lr_arena_new(a, lr_block_t);
    b->name = lr_arena_strdup(a, name, strlen(name));
    b->id = f->num_blocks++;
    b->func = f;
    f->block_array = NULL;
    f->linear_inst_array = NULL;
    f->block_inst_offsets = NULL;
    f->num_linear_insts = 0;
    if (!f->first_block) {
        f->first_block = b;
        f->is_decl = false;
    } else f->last_block->next = b;
    f->last_block = b;
    return b;
}

uint32_t lr_vreg_new(lr_func_t *f) {
    return f->next_vreg++;
}

static size_t align_up_size(size_t value, size_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

lr_inst_t *lr_inst_create(lr_arena_t *a, lr_opcode_t op, lr_type_t *type,
                           uint32_t dest, lr_operand_t *ops, uint32_t nops) {
    size_t operands_offset = align_up_size(sizeof(lr_inst_t), _Alignof(lr_operand_t));
    size_t total_size = operands_offset + sizeof(lr_operand_t) * nops;
    lr_inst_t *inst = (lr_inst_t *)lr_arena_alloc_uninit(a, total_size, _Alignof(lr_inst_t));
    if (!inst)
        return NULL;

    inst->op = op;
    inst->type = type;
    inst->dest = dest;

    if (nops > 0) {
        inst->operands = (lr_operand_t *)((uint8_t *)inst + operands_offset);
        memcpy(inst->operands, ops, sizeof(lr_operand_t) * nops);
    } else {
        inst->operands = NULL;
    }

    inst->num_operands = nops;
    inst->indices = NULL;
    inst->num_indices = 0;
    inst->call_external_abi = false;
    inst->call_vararg = false;
    inst->call_fixed_args = 0;
    inst->next = NULL;
    return inst;
}

void lr_block_append(lr_block_t *b, lr_inst_t *inst) {
    lr_func_t *f = b ? b->func : NULL;
    if (!b->first) b->first = inst;
    else b->last->next = inst;
    b->last = inst;
    b->inst_array = NULL;
    b->num_insts = 0;
    if (f) {
        f->linear_inst_array = NULL;
        f->block_inst_offsets = NULL;
        f->num_linear_insts = 0;
    }
}

typedef struct lr_opt_replacement {
    bool known;
    lr_operand_t op;
} lr_opt_replacement_t;

typedef struct lr_load_cache_entry {
    lr_operand_t ptr;
    lr_type_t *load_type;
    uint32_t value_vreg;
} lr_load_cache_entry_t;

static uint8_t int_type_width_bits(const lr_type_t *type) {
    size_t fallback_bits = 64;
    if (!type)
        return (uint8_t)fallback_bits;

    switch (type->kind) {
    case LR_TYPE_I1:  return 1;
    case LR_TYPE_I8:  return 8;
    case LR_TYPE_I16: return 16;
    case LR_TYPE_I32: return 32;
    case LR_TYPE_I64: return 64;
    default:
        break;
    }

    fallback_bits = lr_type_size(type) * 8;
    if (fallback_bits == 0 || fallback_bits > 64)
        fallback_bits = 64;
    return (uint8_t)fallback_bits;
}

static uint64_t int_mask_for_bits(uint8_t bits) {
    if (bits >= 64)
        return UINT64_MAX;
    if (bits == 0)
        return 0;
    return (UINT64_C(1) << bits) - UINT64_C(1);
}

static uint64_t int_to_unsigned_bits(int64_t val, uint8_t bits) {
    return ((uint64_t)val) & int_mask_for_bits(bits);
}

static int64_t int_sign_extend_bits(uint64_t val, uint8_t bits) {
    if (bits >= 64)
        return (int64_t)val;
    if (bits == 0)
        return 0;
    uint64_t mask = int_mask_for_bits(bits);
    uint64_t sign = UINT64_C(1) << (bits - 1u);
    uint64_t v = val & mask;
    return (int64_t)((v ^ sign) - sign);
}

static bool operand_equal(const lr_operand_t *a, const lr_operand_t *b) {
    uint64_t a_bits = 0;
    uint64_t b_bits = 0;

    if (!a || !b)
        return false;
    if (a->kind != b->kind || a->type != b->type || a->global_offset != b->global_offset)
        return false;

    switch (a->kind) {
    case LR_VAL_VREG:   return a->vreg == b->vreg;
    case LR_VAL_IMM_I64:return a->imm_i64 == b->imm_i64;
    case LR_VAL_IMM_F64:
        memcpy(&a_bits, &a->imm_f64, sizeof(a_bits));
        memcpy(&b_bits, &b->imm_f64, sizeof(b_bits));
        return a_bits == b_bits;
    case LR_VAL_BLOCK:  return a->block_id == b->block_id;
    case LR_VAL_GLOBAL: return a->global_id == b->global_id;
    case LR_VAL_NULL:
    case LR_VAL_UNDEF:
        return true;
    default:
        return false;
    }
}

static bool operand_resolve(const lr_opt_replacement_t *repl, uint32_t nrepl,
                            lr_operand_t *op) {
    bool changed = false;
    uint32_t guard = 0;

    if (!repl || !op)
        return false;

    while (op->kind == LR_VAL_VREG && op->vreg < nrepl &&
           repl[op->vreg].known && guard++ <= nrepl) {
        lr_operand_t next = repl[op->vreg].op;
        if (next.kind == LR_VAL_VREG && next.vreg == op->vreg)
            break;
        *op = next;
        changed = true;
    }

    return changed;
}

static bool inst_defines_dest(const lr_inst_t *inst) {
    if (!inst)
        return false;

    switch (inst->op) {
    case LR_OP_RET:
    case LR_OP_RET_VOID:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_UNREACHABLE:
    case LR_OP_STORE:
        return false;
    case LR_OP_CALL:
        return inst->type && inst->type->kind != LR_TYPE_VOID;
    default:
        return inst->type && inst->type->kind != LR_TYPE_VOID;
    }
}

static bool inst_dead_def_eliminable(const lr_inst_t *inst) {
    if (!inst_defines_dest(inst))
        return false;

    switch (inst->op) {
    case LR_OP_ALLOCA:
    case LR_OP_LOAD:
    case LR_OP_CALL:
        return false;
    default:
        return true;
    }
}

static bool fold_int_binop_immediates(const lr_inst_t *inst, lr_operand_t *out) {
    const lr_operand_t *lhs;
    const lr_operand_t *rhs;
    uint8_t bits;
    uint64_t mask;
    uint64_t u_lhs;
    uint64_t u_rhs;
    uint64_t u_res = 0;
    int64_t s_lhs;
    int64_t s_rhs;
    int64_t s_res;

    if (!inst || !out || inst->num_operands < 2)
        return false;

    lhs = &inst->operands[0];
    rhs = &inst->operands[1];
    if (lhs->kind != LR_VAL_IMM_I64 || rhs->kind != LR_VAL_IMM_I64)
        return false;

    bits = int_type_width_bits(inst->type);
    mask = int_mask_for_bits(bits);
    u_lhs = int_to_unsigned_bits(lhs->imm_i64, bits);
    u_rhs = int_to_unsigned_bits(rhs->imm_i64, bits);
    s_lhs = int_sign_extend_bits(u_lhs, bits);
    s_rhs = int_sign_extend_bits(u_rhs, bits);

    switch (inst->op) {
    case LR_OP_ADD:
        u_res = (u_lhs + u_rhs) & mask;
        break;
    case LR_OP_SUB:
        u_res = (u_lhs - u_rhs) & mask;
        break;
    case LR_OP_MUL:
        u_res = (u_lhs * u_rhs) & mask;
        break;
    case LR_OP_AND:
        u_res = (u_lhs & u_rhs) & mask;
        break;
    case LR_OP_OR:
        u_res = (u_lhs | u_rhs) & mask;
        break;
    case LR_OP_XOR:
        u_res = (u_lhs ^ u_rhs) & mask;
        break;
    case LR_OP_SHL:
        if (u_rhs >= bits)
            return false;
        u_res = (u_lhs << u_rhs) & mask;
        break;
    case LR_OP_LSHR:
        if (u_rhs >= bits)
            return false;
        u_res = (u_lhs >> u_rhs) & mask;
        break;
    case LR_OP_ASHR:
        if (u_rhs >= bits)
            return false;
        s_res = (s_lhs >> u_rhs);
        u_res = ((uint64_t)s_res) & mask;
        break;
    case LR_OP_SDIV:
        if (s_rhs == 0)
            return false;
        if (bits == 64 && s_lhs == INT64_MIN && s_rhs == -1)
            return false;
        s_res = s_lhs / s_rhs;
        u_res = ((uint64_t)s_res) & mask;
        break;
    case LR_OP_SREM:
        if (s_rhs == 0)
            return false;
        if (bits == 64 && s_lhs == INT64_MIN && s_rhs == -1)
            return false;
        s_res = s_lhs % s_rhs;
        u_res = ((uint64_t)s_res) & mask;
        break;
    default:
        return false;
    }

    *out = lr_op_imm_i64(int_sign_extend_bits(u_res, bits), inst->type);
    return true;
}

static bool fold_icmp_immediates(const lr_inst_t *inst, lr_operand_t *out) {
    const lr_operand_t *lhs;
    const lr_operand_t *rhs;
    lr_type_t *cmp_ty;
    uint8_t bits;
    uint64_t u_lhs;
    uint64_t u_rhs;
    int64_t s_lhs;
    int64_t s_rhs;
    int64_t pred = 0;

    if (!inst || !out || inst->op != LR_OP_ICMP || inst->num_operands < 2)
        return false;

    lhs = &inst->operands[0];
    rhs = &inst->operands[1];
    if (lhs->kind != LR_VAL_IMM_I64 || rhs->kind != LR_VAL_IMM_I64)
        return false;

    cmp_ty = lhs->type ? lhs->type : inst->type;
    bits = int_type_width_bits(cmp_ty);
    u_lhs = int_to_unsigned_bits(lhs->imm_i64, bits);
    u_rhs = int_to_unsigned_bits(rhs->imm_i64, bits);
    s_lhs = int_sign_extend_bits(u_lhs, bits);
    s_rhs = int_sign_extend_bits(u_rhs, bits);

    switch (inst->icmp_pred) {
    case LR_ICMP_EQ: pred = (u_lhs == u_rhs); break;
    case LR_ICMP_NE: pred = (u_lhs != u_rhs); break;
    case LR_ICMP_SGT: pred = (s_lhs > s_rhs); break;
    case LR_ICMP_SGE: pred = (s_lhs >= s_rhs); break;
    case LR_ICMP_SLT: pred = (s_lhs < s_rhs); break;
    case LR_ICMP_SLE: pred = (s_lhs <= s_rhs); break;
    case LR_ICMP_UGT: pred = (u_lhs > u_rhs); break;
    case LR_ICMP_UGE: pred = (u_lhs >= u_rhs); break;
    case LR_ICMP_ULT: pred = (u_lhs < u_rhs); break;
    case LR_ICMP_ULE: pred = (u_lhs <= u_rhs); break;
    default:
        return false;
    }

    *out = lr_op_imm_i64(pred ? 1 : 0, inst->type);
    return true;
}

static bool fold_identity_int_binop(const lr_inst_t *inst, lr_operand_t *out) {
    const lr_operand_t *lhs;
    const lr_operand_t *rhs;
    uint8_t bits;
    uint64_t mask;

    if (!inst || !out || inst->num_operands < 2)
        return false;

    lhs = &inst->operands[0];
    rhs = &inst->operands[1];
    bits = int_type_width_bits(inst->type);
    mask = int_mask_for_bits(bits);

    switch (inst->op) {
    case LR_OP_ADD:
        if (lhs->kind == LR_VAL_IMM_I64 && lhs->imm_i64 == 0) { *out = *rhs; return true; }
        if (rhs->kind == LR_VAL_IMM_I64 && rhs->imm_i64 == 0) { *out = *lhs; return true; }
        return false;
    case LR_OP_SUB:
        if (rhs->kind == LR_VAL_IMM_I64 && rhs->imm_i64 == 0) { *out = *lhs; return true; }
        return false;
    case LR_OP_MUL:
        if (lhs->kind == LR_VAL_IMM_I64 && lhs->imm_i64 == 1) { *out = *rhs; return true; }
        if (rhs->kind == LR_VAL_IMM_I64 && rhs->imm_i64 == 1) { *out = *lhs; return true; }
        return false;
    case LR_OP_SDIV:
        if (rhs->kind == LR_VAL_IMM_I64 && rhs->imm_i64 == 1) { *out = *lhs; return true; }
        return false;
    case LR_OP_AND:
        if (lhs->kind == LR_VAL_IMM_I64 &&
            (((uint64_t)lhs->imm_i64 & mask) == mask)) { *out = *rhs; return true; }
        if (rhs->kind == LR_VAL_IMM_I64 &&
            (((uint64_t)rhs->imm_i64 & mask) == mask)) { *out = *lhs; return true; }
        return false;
    case LR_OP_OR:
    case LR_OP_XOR:
        if (lhs->kind == LR_VAL_IMM_I64 && lhs->imm_i64 == 0) { *out = *rhs; return true; }
        if (rhs->kind == LR_VAL_IMM_I64 && rhs->imm_i64 == 0) { *out = *lhs; return true; }
        return false;
    case LR_OP_SHL:
    case LR_OP_LSHR:
    case LR_OP_ASHR:
        if (rhs->kind == LR_VAL_IMM_I64 && rhs->imm_i64 == 0) { *out = *lhs; return true; }
        return false;
    default:
        return false;
    }
}

static bool fold_select(const lr_inst_t *inst, lr_operand_t *out) {
    if (!inst || !out || inst->op != LR_OP_SELECT || inst->num_operands < 3)
        return false;

    if (inst->operands[0].kind == LR_VAL_IMM_I64) {
        *out = inst->operands[inst->operands[0].imm_i64 ? 1u : 2u];
        return true;
    }

    if (operand_equal(&inst->operands[1], &inst->operands[2])) {
        *out = inst->operands[1];
        return true;
    }

    return false;
}

static bool try_inst_replacement(const lr_inst_t *inst, lr_operand_t *out) {
    if (fold_select(inst, out))
        return true;
    if (fold_icmp_immediates(inst, out))
        return true;
    if (fold_int_binop_immediates(inst, out))
        return true;
    if (fold_identity_int_binop(inst, out))
        return true;
    return false;
}

static int run_func_peephole_passes(lr_func_t *f, lr_arena_t *a) {
    uint32_t nrepl;
    lr_opt_replacement_t *repl;
    lr_load_cache_entry_t *load_cache;
    uint32_t *use_counts;
    bool changed_any = false;

    if (!f || !a || f->num_blocks == 0)
        return 0;

    nrepl = f->next_vreg > 0 ? f->next_vreg : 1u;
    repl = lr_arena_array(a, lr_opt_replacement_t, nrepl);
    load_cache = lr_arena_array(a, lr_load_cache_entry_t, nrepl);
    use_counts = lr_arena_array(a, uint32_t, nrepl);
    if (!repl || !load_cache || !use_counts)
        return -1;

    for (uint32_t iter = 0; iter < 6; iter++) {
        bool iter_changed = false;

        for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
            lr_block_t *b = f->block_array[bi];
            lr_inst_t *prev = NULL;
            uint32_t load_count = 0;

            if (!b)
                return -1;

            for (lr_inst_t *inst = b->first; inst; ) {
                lr_inst_t *next = inst->next;
                bool remove_inst = false;
                lr_operand_t replacement = {0};

                for (uint32_t oi = 0; oi < inst->num_operands; oi++) {
                    if (operand_resolve(repl, nrepl, &inst->operands[oi]))
                        iter_changed = true;
                }

                if (inst->op == LR_OP_CONDBR &&
                    inst->num_operands >= 3 &&
                    inst->operands[0].kind == LR_VAL_IMM_I64) {
                    lr_operand_t target = inst->operands[inst->operands[0].imm_i64 ? 1u : 2u];
                    inst->op = LR_OP_BR;
                    inst->num_operands = 1;
                    inst->operands[0] = target;
                    iter_changed = true;
                }

                if (try_inst_replacement(inst, &replacement)) {
                    remove_inst = true;
                } else if (inst->op == LR_OP_LOAD &&
                           inst->num_operands >= 1 &&
                           inst_defines_dest(inst)) {
                    for (uint32_t li = 0; li < load_count; li++) {
                        if (load_cache[li].load_type == inst->type &&
                            operand_equal(&load_cache[li].ptr, &inst->operands[0])) {
                            replacement = lr_op_vreg(load_cache[li].value_vreg, inst->type);
                            remove_inst = true;
                            break;
                        }
                    }
                    if (!remove_inst && load_count < nrepl) {
                        load_cache[load_count].ptr = inst->operands[0];
                        load_cache[load_count].load_type = inst->type;
                        load_cache[load_count].value_vreg = inst->dest;
                        load_count++;
                    }
                }

                if (!remove_inst &&
                    (inst->op == LR_OP_STORE || inst->op == LR_OP_CALL))
                    load_count = 0;

                if (remove_inst && inst_defines_dest(inst) && inst->dest < nrepl) {
                    if (!(replacement.kind == LR_VAL_VREG &&
                          replacement.vreg == inst->dest)) {
                        if (replacement.type == NULL &&
                            (replacement.kind == LR_VAL_VREG ||
                             replacement.kind == LR_VAL_IMM_I64 ||
                             replacement.kind == LR_VAL_IMM_F64 ||
                             replacement.kind == LR_VAL_NULL ||
                             replacement.kind == LR_VAL_UNDEF)) {
                            replacement.type = inst->type;
                        }
                        repl[inst->dest].known = true;
                        repl[inst->dest].op = replacement;
                    }

                    if (prev)
                        prev->next = next;
                    else
                        b->first = next;
                    if (b->last == inst)
                        b->last = prev;

                    iter_changed = true;
                    changed_any = true;
                    inst = next;
                    continue;
                }

                prev = inst;
                inst = next;
            }

            if (!b->first)
                b->last = NULL;
        }

        if (!iter_changed)
            break;
    }

    for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
        lr_block_t *b = f->block_array[bi];
        if (!b)
            return -1;
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            for (uint32_t oi = 0; oi < inst->num_operands; oi++)
                operand_resolve(repl, nrepl, &inst->operands[oi]);
        }
    }

    for (uint32_t iter = 0; iter < 8; iter++) {
        bool removed_any = false;
        memset(use_counts, 0, sizeof(uint32_t) * nrepl);

        for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
            lr_block_t *b = f->block_array[bi];
            if (!b)
                return -1;
            for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
                for (uint32_t oi = 0; oi < inst->num_operands; oi++) {
                    if (inst->operands[oi].kind == LR_VAL_VREG &&
                        inst->operands[oi].vreg < nrepl)
                        use_counts[inst->operands[oi].vreg]++;
                }
            }
        }

        for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
            lr_block_t *b = f->block_array[bi];
            lr_inst_t *prev = NULL;
            if (!b)
                return -1;

            for (lr_inst_t *inst = b->first; inst; ) {
                lr_inst_t *next = inst->next;
                if (inst_dead_def_eliminable(inst) &&
                    inst->dest < nrepl &&
                    use_counts[inst->dest] == 0) {
                    if (prev)
                        prev->next = next;
                    else
                        b->first = next;
                    if (b->last == inst)
                        b->last = prev;
                    removed_any = true;
                    changed_any = true;
                    inst = next;
                    continue;
                }
                prev = inst;
                inst = next;
            }
            if (!b->first)
                b->last = NULL;
        }

        if (!removed_any)
            break;
    }

    (void)changed_any;
    return 0;
}

int lr_func_finalize(lr_func_t *f, lr_arena_t *a) {
    if (!f || !a)
        return -1;

    if (lr_func_is_finalized(f))
        return 0;

    if (f->num_blocks == 0)
        return 0;

    if (!f->block_array) {
        uint32_t seen = 0;
        f->block_array = lr_arena_array(a, lr_block_t *, f->num_blocks);
        if (!f->block_array)
            return -1;
        for (lr_block_t *b = f->first_block; b; b = b->next) {
            if (b->id >= f->num_blocks)
                return -1;
            f->block_array[b->id] = b;
            seen++;
        }
        if (seen != f->num_blocks)
            return -1;
        for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
            if (!f->block_array[bi])
                return -1;
        }
    }

    if (run_func_peephole_passes(f, a) != 0)
        return -1;

    for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
        lr_block_t *b = f->block_array[bi];
        uint32_t n = 0, j = 0;
        if (!b)
            return -1;

        for (lr_inst_t *inst = b->first; inst; inst = inst->next)
            n++;
        b->num_insts = n;
        b->inst_array = NULL;
        if (n == 0)
            continue;

        b->inst_array = lr_arena_array(a, lr_inst_t *, n);
        if (!b->inst_array)
            return -1;
        for (lr_inst_t *inst = b->first; inst; inst = inst->next)
            b->inst_array[j++] = inst;
    }

    if (!f->block_inst_offsets) {
        f->block_inst_offsets = lr_arena_array(a, uint32_t, f->num_blocks + 1u);
        if (!f->block_inst_offsets)
            return -1;
    }

    if (!f->linear_inst_array) {
        uint32_t at = 0;
        uint32_t total = 0;

        for (uint32_t bi = 0; bi < f->num_blocks; bi++)
            total += f->block_array[bi]->num_insts;

        f->num_linear_insts = total;
        if (total > 0) {
            f->linear_inst_array = lr_arena_array(a, lr_inst_t *, total);
            if (!f->linear_inst_array)
                return -1;
        }

        for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
            lr_block_t *b = f->block_array[bi];
            f->block_inst_offsets[bi] = at;
            for (uint32_t ii = 0; ii < b->num_insts; ii++)
                f->linear_inst_array[at++] = b->inst_array[ii];
        }
        f->block_inst_offsets[f->num_blocks] = at;
    }

    return 0;
}

bool lr_func_is_finalized(const lr_func_t *f) {
    bool has_insts = false;

    if (!f)
        return false;

    if (f->num_blocks == 0)
        return true;

    if (!f->block_array || !f->block_inst_offsets)
        return false;

    for (uint32_t bi = 0; bi < f->num_blocks; bi++) {
        lr_block_t *b = f->block_array[bi];
        if (!b)
            return false;
        if (b->first && !b->inst_array)
            return false;
        if (b->first)
            has_insts = true;
    }

    if (has_insts && !f->linear_inst_array)
        return false;
    if (!has_insts && f->num_linear_insts != 0)
        return false;

    return f->block_inst_offsets[f->num_blocks] == f->num_linear_insts;
}

lr_global_t *lr_global_create(lr_module_t *m, const char *name, lr_type_t *type,
                               bool is_const) {
    lr_arena_t *a = m->arena;
    lr_global_t *g = lr_arena_new(a, lr_global_t);
    g->name = lr_arena_strdup(a, name, strlen(name));
    g->type = type;
    g->is_const = is_const;
    g->id = m->num_globals++;
    if (!m->first_global) m->first_global = g;
    else m->last_global->next = g;
    m->last_global = g;
    return g;
}

lr_operand_t lr_op_vreg(uint32_t vreg, lr_type_t *type) {
    return (lr_operand_t){
        .kind = LR_VAL_VREG, .vreg = vreg, .type = type, .global_offset = 0
    };
}

lr_operand_t lr_op_imm_i64(int64_t val, lr_type_t *type) {
    return (lr_operand_t){
        .kind = LR_VAL_IMM_I64, .imm_i64 = val, .type = type, .global_offset = 0
    };
}

lr_operand_t lr_op_imm_f64(double val, lr_type_t *type) {
    return (lr_operand_t){
        .kind = LR_VAL_IMM_F64, .imm_f64 = val, .type = type, .global_offset = 0
    };
}

lr_operand_t lr_op_block(uint32_t id) {
    return (lr_operand_t){
        .kind = LR_VAL_BLOCK, .block_id = id, .global_offset = 0
    };
}

lr_operand_t lr_op_global(uint32_t id, lr_type_t *type) {
    return (lr_operand_t){
        .kind = LR_VAL_GLOBAL, .global_id = id, .type = type, .global_offset = 0
    };
}

lr_operand_t lr_op_null(lr_type_t *type) {
    return (lr_operand_t){ .kind = LR_VAL_NULL, .type = type, .global_offset = 0 };
}

uint32_t lr_module_intern_symbol(lr_module_t *m, const char *name) {
    uint32_t hash = symbol_hash(name);
    uint32_t slot;

    if (m->symbol_index_cap == 0) {
        if (symbol_index_rebuild(m, 1) != 0)
            return UINT32_MAX;
    }

    slot = hash & (m->symbol_index_cap - 1u);
    for (;;) {
        uint32_t stored = m->symbol_index[slot];
        if (stored == 0)
            break;

        uint32_t i = stored - 1u;
        if (m->symbol_hashes[i] == hash && strcmp(m->symbol_names[i], name) == 0)
            return i;
        slot = (slot + 1u) & (m->symbol_index_cap - 1u);
    }

    if (m->num_symbols == m->symbol_cap) {
        uint32_t old_cap = m->symbol_cap;
        uint32_t new_cap = old_cap == 0 ? 64 : old_cap * 2;
        char **names = lr_arena_array(m->arena, char *, new_cap);
        uint32_t *hashes = lr_arena_array(m->arena, uint32_t, new_cap);
        if (old_cap > 0)
            memcpy(names, m->symbol_names, sizeof(char *) * old_cap);
        if (old_cap > 0)
            memcpy(hashes, m->symbol_hashes, sizeof(uint32_t) * old_cap);
        m->symbol_names = names;
        m->symbol_hashes = hashes;
        m->symbol_cap = new_cap;
    }

    if ((m->num_symbols + 1u) * 2u > m->symbol_index_cap) {
        if (symbol_index_rebuild(m, m->num_symbols + 1u) != 0)
            return UINT32_MAX;
    }

    uint32_t id = m->num_symbols++;
    m->symbol_names[id] = lr_arena_strdup(m->arena, name, strlen(name));
    m->symbol_hashes[id] = hash;

    slot = hash & (m->symbol_index_cap - 1u);
    while (m->symbol_index[slot] != 0)
        slot = (slot + 1u) & (m->symbol_index_cap - 1u);
    m->symbol_index[slot] = id + 1u;

    return id;
}

const char *lr_module_symbol_name(const lr_module_t *m, uint32_t id) {
    if (!m || id >= m->num_symbols)
        return NULL;
    return m->symbol_names[id];
}

size_t lr_type_size(const lr_type_t *t) {
    if (!t) return 0;
    switch (t->kind) {
    case LR_TYPE_VOID:   return 0;
    case LR_TYPE_I1:     return 1;
    case LR_TYPE_I8:     return 1;
    case LR_TYPE_I16:    return 2;
    case LR_TYPE_I32:    return 4;
    case LR_TYPE_I64:    return 8;
    case LR_TYPE_FLOAT:  return 4;
    case LR_TYPE_DOUBLE: return 8;
    case LR_TYPE_PTR:    return 8;
    case LR_TYPE_ARRAY:  return lr_type_size(t->array.elem) * t->array.count;
    case LR_TYPE_STRUCT: {
        size_t sz = 0;
        for (uint32_t i = 0; i < t->struc.num_fields; i++) {
            size_t fsz = lr_type_size(t->struc.fields[i]);
            if (!t->struc.packed) {
                size_t fa = lr_type_align(t->struc.fields[i]);
                sz = (sz + fa - 1) & ~(fa - 1);
            }
            sz += fsz;
        }
        if (!t->struc.packed && t->struc.num_fields > 0) {
            size_t sa = lr_type_align(t);
            sz = (sz + sa - 1) & ~(sa - 1);
        }
        return sz;
    }
    case LR_TYPE_FUNC: return 0;
    }
    return 0;
}

size_t lr_type_align(const lr_type_t *t) {
    if (!t) return 1;
    switch (t->kind) {
    case LR_TYPE_VOID:   return 1;
    case LR_TYPE_I1:     return 1;
    case LR_TYPE_I8:     return 1;
    case LR_TYPE_I16:    return 2;
    case LR_TYPE_I32:    return 4;
    case LR_TYPE_I64:    return 8;
    case LR_TYPE_FLOAT:  return 4;
    case LR_TYPE_DOUBLE: return 8;
    case LR_TYPE_PTR:    return 8;
    case LR_TYPE_ARRAY:  return lr_type_align(t->array.elem);
    case LR_TYPE_STRUCT: {
        if (t->struc.packed) return 1;
        size_t max_a = 1;
        for (uint32_t i = 0; i < t->struc.num_fields; i++) {
            size_t fa = lr_type_align(t->struc.fields[i]);
            if (fa > max_a) max_a = fa;
        }
        return max_a;
    }
    case LR_TYPE_FUNC: return 1;
    }
    return 1;
}

size_t lr_struct_field_offset(const lr_type_t *st, uint32_t field_idx) {
    size_t off = 0;
    if (!st || st->kind != LR_TYPE_STRUCT)
        return 0;
    for (uint32_t i = 0; i < st->struc.num_fields && i < field_idx; i++) {
        if (!st->struc.packed) {
            size_t fa = lr_type_align(st->struc.fields[i]);
            off = (off + fa - 1) & ~(fa - 1);
        }
        off += lr_type_size(st->struc.fields[i]);
    }
    if (field_idx < st->struc.num_fields && !st->struc.packed) {
        size_t fa = lr_type_align(st->struc.fields[field_idx]);
        off = (off + fa - 1) & ~(fa - 1);
    }
    return off;
}

bool lr_aggregate_index_path(const lr_type_t *base, const uint32_t *indices,
                             uint32_t num_indices, size_t *byte_offset_out,
                             const lr_type_t **leaf_type_out) {
    size_t off = 0;
    const lr_type_t *cur = base;

    if (!base) {
        return false;
    }
    if (num_indices > 0 && !indices) {
        return false;
    }

    for (uint32_t i = 0; i < num_indices; i++) {
        uint32_t idx = indices[i];
        if (!cur) {
            return false;
        }

        if (cur->kind == LR_TYPE_STRUCT) {
            if (idx >= cur->struc.num_fields) {
                return false;
            }
            off += lr_struct_field_offset(cur, idx);
            cur = cur->struc.fields[idx];
            continue;
        }

        if (cur->kind == LR_TYPE_ARRAY) {
            if (idx >= cur->array.count) {
                return false;
            }
            off += (size_t)idx * lr_type_size(cur->array.elem);
            cur = cur->array.elem;
            continue;
        }

        return false;
    }

    if (byte_offset_out) {
        *byte_offset_out = off;
    }
    if (leaf_type_out) {
        *leaf_type_out = cur;
    }
    return true;
}

lr_block_phi_copies_t *lr_build_phi_copies(lr_arena_t *arena, lr_func_t *func) {
    if (!arena || !func)
        return NULL;
    if (!lr_func_is_finalized(func) && lr_func_finalize(func, arena) != 0)
        return NULL;

    lr_block_phi_copies_t *blocks =
        lr_arena_array(arena, lr_block_phi_copies_t, func->num_blocks);
    uint32_t *fill_pos = lr_arena_array(arena, uint32_t, func->num_blocks);

    for (uint32_t i = 0; i < func->num_blocks; i++) {
        blocks[i].copies = NULL;
        blocks[i].count = 0;
        fill_pos[i] = 0;
    }

    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        lr_block_t *b = func->block_array[bi];
        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            if (inst->op != LR_OP_PHI)
                continue;
            for (uint32_t i = 0; i + 1 < inst->num_operands; i += 2) {
                uint32_t pred_id = inst->operands[i + 1].block_id;
                if (pred_id >= func->num_blocks)
                    continue;
                blocks[pred_id].count++;
            }
        }
    }

    for (uint32_t i = 0; i < func->num_blocks; i++) {
        if (blocks[i].count == 0)
            continue;
        blocks[i].copies = lr_arena_array(arena, lr_phi_copy_t, blocks[i].count);
        fill_pos[i] = blocks[i].count;
    }

    for (uint32_t bi = 0; bi < func->num_blocks; bi++) {
        lr_block_t *b = func->block_array[bi];
        for (uint32_t ii = 0; ii < b->num_insts; ii++) {
            lr_inst_t *inst = b->inst_array[ii];
            if (inst->op != LR_OP_PHI)
                continue;
            for (uint32_t i = 0; i + 1 < inst->num_operands; i += 2) {
                uint32_t pred_id = inst->operands[i + 1].block_id;
                if (pred_id >= func->num_blocks)
                    continue;
                uint32_t slot = --fill_pos[pred_id];
                blocks[pred_id].copies[slot].dest_vreg = inst->dest;
                blocks[pred_id].copies[slot].src_op = inst->operands[i];
            }
        }
    }

    return blocks;
}

uint8_t lr_gep_index_signext_bytes(const lr_operand_t *idx_op) {
    if (!idx_op || !idx_op->type)
        return 0;
    switch (idx_op->type->kind) {
    case LR_TYPE_I1:
    case LR_TYPE_I8:
        return 1;
    case LR_TYPE_I16:
        return 2;
    case LR_TYPE_I32:
        return 4;
    default:
        return 0;
    }
}

bool lr_gep_analyze_step(const lr_type_t *cur_ty, bool first_index,
                         const lr_operand_t *idx_op, lr_gep_step_t *out) {
    if (!cur_ty || !idx_op || !out)
        return false;

    out->is_const = false;
    out->const_byte_offset = 0;
    out->runtime_elem_size = 0;
    out->runtime_signext_bytes = 0;
    out->next_type = cur_ty;

    if (first_index) {
        size_t elem_size = lr_type_size(cur_ty);
        out->next_type = cur_ty;
        if (idx_op->kind == LR_VAL_IMM_I64) {
            out->is_const = true;
            out->const_byte_offset = idx_op->imm_i64 * (int64_t)elem_size;
        } else {
            out->runtime_elem_size = elem_size;
            out->runtime_signext_bytes = lr_gep_index_signext_bytes(idx_op);
        }
        return idx_op;
    }

    if (cur_ty->kind == LR_TYPE_STRUCT) {
        uint32_t field = (idx_op->kind == LR_VAL_IMM_I64)
                             ? (uint32_t)idx_op->imm_i64
                             : (uint32_t)idx_op->vreg;
        out->is_const = true;
        out->const_byte_offset = (int64_t)lr_struct_field_offset(cur_ty, field);
        if (field < cur_ty->struc.num_fields)
            out->next_type = cur_ty->struc.fields[field];
        return true;
    }

    if (cur_ty->kind == LR_TYPE_ARRAY) {
        size_t elem_size = lr_type_size(cur_ty->array.elem);
        out->next_type = cur_ty->array.elem;
        if (idx_op->kind == LR_VAL_IMM_I64) {
            out->is_const = true;
            out->const_byte_offset = idx_op->imm_i64 * (int64_t)elem_size;
        } else {
            out->runtime_elem_size = elem_size;
            out->runtime_signext_bytes = lr_gep_index_signext_bytes(idx_op);
        }
        return true;
    }

    return false;
}

lr_operand_t lr_canonicalize_gep_index(lr_module_t *m, lr_block_t *b,
                                       lr_func_t *f, lr_operand_t idx_op) {
    if (!m) {
        return idx_op;
    }

    if (idx_op.kind == LR_VAL_IMM_I64 || idx_op.kind == LR_VAL_UNDEF) {
        if (idx_op.type != m->type_i64) {
            idx_op.type = m->type_i64;
        }
        return idx_op;
    }

    if (!idx_op.type || idx_op.type->kind == LR_TYPE_I64) {
        return idx_op;
    }

    if (idx_op.kind != LR_VAL_VREG || !b || !f) {
        return idx_op;
    }

    switch (idx_op.type->kind) {
    case LR_TYPE_I1:
    case LR_TYPE_I8:
    case LR_TYPE_I16:
    case LR_TYPE_I32: {
        uint32_t cast_dest = lr_vreg_new(f);
        lr_operand_t cast_ops[1] = { idx_op };
        lr_inst_t *cast = lr_inst_create(m->arena, LR_OP_SEXT,
                                         m->type_i64, cast_dest, cast_ops, 1);
        lr_block_append(b, cast);
        return lr_op_vreg(cast_dest, m->type_i64);
    }
    default:
        return idx_op;
    }
}

static const char *type_name(const lr_type_t *t) {
    switch (t->kind) {
    case LR_TYPE_VOID:   return "void";
    case LR_TYPE_I1:     return "i1";
    case LR_TYPE_I8:     return "i8";
    case LR_TYPE_I16:    return "i16";
    case LR_TYPE_I32:    return "i32";
    case LR_TYPE_I64:    return "i64";
    case LR_TYPE_FLOAT:  return "float";
    case LR_TYPE_DOUBLE: return "double";
    case LR_TYPE_PTR:    return "ptr";
    default: return "?";
    }
    return "?";
}

static void print_type(const lr_type_t *t, FILE *out) {
    if (t->kind <= LR_TYPE_PTR) {
        fprintf(out, "%s", type_name(t));
    } else if (t->kind == LR_TYPE_ARRAY) {
        fprintf(out, "[%lu x ", (unsigned long)t->array.count);
        print_type(t->array.elem, out);
        fprintf(out, "]");
    } else if (t->kind == LR_TYPE_STRUCT) {
        fprintf(out, "{ ");
        for (uint32_t i = 0; i < t->struc.num_fields; i++) {
            if (i > 0) fprintf(out, ", ");
            print_type(t->struc.fields[i], out);
        }
        fprintf(out, " }");
    }
}

static void print_operand(const lr_operand_t *op, const lr_module_t *m,
                          FILE *out) {
    switch (op->kind) {
    case LR_VAL_VREG:    fprintf(out, "%%v%u", op->vreg); break;
    case LR_VAL_IMM_I64: fprintf(out, "%ld", (long)op->imm_i64); break;
    case LR_VAL_IMM_F64: fprintf(out, "%g", op->imm_f64); break;
    case LR_VAL_BLOCK:   fprintf(out, "label %%bb%u", op->block_id); break;
    case LR_VAL_GLOBAL: {
        const char *name = lr_module_symbol_name(m, op->global_id);
        if (name) fprintf(out, "@%s", name);
        else fprintf(out, "@g%u", op->global_id);
        if (op->global_offset > 0)
            fprintf(out, "+%ld", (long)op->global_offset);
        else if (op->global_offset < 0)
            fprintf(out, "%ld", (long)op->global_offset);
        break;
    }
    case LR_VAL_NULL:    fprintf(out, "null"); break;
    case LR_VAL_UNDEF:   fprintf(out, "undef"); break;
    }
}

static const char *opcode_name(lr_opcode_t op) {
    switch (op) {
    case LR_OP_RET:          return "ret";
    case LR_OP_RET_VOID:     return "ret void";
    case LR_OP_BR:           return "br";
    case LR_OP_CONDBR:       return "br";
    case LR_OP_UNREACHABLE:  return "unreachable";
    case LR_OP_ADD:          return "add";
    case LR_OP_SUB:          return "sub";
    case LR_OP_MUL:          return "mul";
    case LR_OP_SDIV:         return "sdiv";
    case LR_OP_SREM:         return "srem";
    case LR_OP_AND:          return "and";
    case LR_OP_OR:           return "or";
    case LR_OP_XOR:          return "xor";
    case LR_OP_SHL:          return "shl";
    case LR_OP_LSHR:         return "lshr";
    case LR_OP_ASHR:         return "ashr";
    case LR_OP_FADD:         return "fadd";
    case LR_OP_FSUB:         return "fsub";
    case LR_OP_FMUL:         return "fmul";
    case LR_OP_FDIV:         return "fdiv";
    case LR_OP_FNEG:         return "fneg";
    case LR_OP_ICMP:         return "icmp";
    case LR_OP_FCMP:         return "fcmp";
    case LR_OP_ALLOCA:       return "alloca";
    case LR_OP_LOAD:         return "load";
    case LR_OP_STORE:        return "store";
    case LR_OP_GEP:          return "getelementptr";
    case LR_OP_CALL:         return "call";
    case LR_OP_PHI:          return "phi";
    case LR_OP_SELECT:       return "select";
    case LR_OP_SEXT:         return "sext";
    case LR_OP_ZEXT:         return "zext";
    case LR_OP_TRUNC:        return "trunc";
    case LR_OP_BITCAST:      return "bitcast";
    case LR_OP_PTRTOINT:     return "ptrtoint";
    case LR_OP_INTTOPTR:     return "inttoptr";
    case LR_OP_SITOFP:       return "sitofp";
    case LR_OP_UITOFP:       return "uitofp";
    case LR_OP_FPTOSI:       return "fptosi";
    case LR_OP_FPTOUI:       return "fptoui";
    case LR_OP_FPEXT:        return "fpext";
    case LR_OP_FPTRUNC:      return "fptrunc";
    case LR_OP_EXTRACTVALUE: return "extractvalue";
    case LR_OP_INSERTVALUE:  return "insertvalue";
    }
    return "?";
}

static const char *icmp_pred_name(int pred) {
    switch (pred) {
    case LR_ICMP_EQ:  return "eq";
    case LR_ICMP_NE:  return "ne";
    case LR_ICMP_SGT: return "sgt";
    case LR_ICMP_SGE: return "sge";
    case LR_ICMP_SLT: return "slt";
    case LR_ICMP_SLE: return "sle";
    case LR_ICMP_UGT: return "ugt";
    case LR_ICMP_UGE: return "uge";
    case LR_ICMP_ULT: return "ult";
    case LR_ICMP_ULE: return "ule";
    }
    return "?";
}

static const char *fcmp_pred_name(int pred) {
    switch (pred) {
    case LR_FCMP_FALSE: return "false";
    case LR_FCMP_OEQ:   return "oeq";
    case LR_FCMP_OGT:   return "ogt";
    case LR_FCMP_OGE:   return "oge";
    case LR_FCMP_OLT:   return "olt";
    case LR_FCMP_OLE:   return "ole";
    case LR_FCMP_ONE:   return "one";
    case LR_FCMP_ORD:   return "ord";
    case LR_FCMP_UEQ:   return "ueq";
    case LR_FCMP_UGT:   return "ugt";
    case LR_FCMP_UGE:   return "uge";
    case LR_FCMP_ULT:   return "ult";
    case LR_FCMP_ULE:   return "ule";
    case LR_FCMP_UNE:   return "une";
    case LR_FCMP_UNO:   return "uno";
    case LR_FCMP_TRUE:  return "true";
    }
    return "?";
}

static bool is_cast_op(lr_opcode_t op) {
    return op == LR_OP_SEXT || op == LR_OP_ZEXT || op == LR_OP_TRUNC ||
           op == LR_OP_BITCAST || op == LR_OP_PTRTOINT ||
           op == LR_OP_INTTOPTR || op == LR_OP_SITOFP || op == LR_OP_UITOFP ||
           op == LR_OP_FPTOSI || op == LR_OP_FPTOUI ||
           op == LR_OP_FPEXT || op == LR_OP_FPTRUNC;
}

static bool is_void_type(const lr_type_t *t) {
    return t && t->kind == LR_TYPE_VOID;
}

static bool inst_has_dest(const lr_inst_t *inst) {
    switch (inst->op) {
    case LR_OP_RET:
    case LR_OP_RET_VOID:
    case LR_OP_BR:
    case LR_OP_CONDBR:
    case LR_OP_STORE:
    case LR_OP_UNREACHABLE:
        return false;
    case LR_OP_CALL:
        return !is_void_type(inst->type);
    default:
        return true;
    }
}

static void dump_inst(const lr_inst_t *inst, const lr_module_t *m, FILE *out) {
    fprintf(out, "  ");
    if (inst_has_dest(inst))
        fprintf(out, "%%v%u = ", inst->dest);
    fprintf(out, "%s ", opcode_name(inst->op));

    switch (inst->op) {
    case LR_OP_RET_VOID:
    case LR_OP_UNREACHABLE:
        break;

    case LR_OP_RET:
        if (inst->num_operands > 0 && inst->operands[0].type)
            print_type(inst->operands[0].type, out);
        else if (inst->type)
            print_type(inst->type, out);
        fprintf(out, " ");
        if (inst->num_operands > 0)
            print_operand(&inst->operands[0], m, out);
        break;

    case LR_OP_BR:
        if (inst->num_operands > 0)
            print_operand(&inst->operands[0], m, out);
        break;

    case LR_OP_CONDBR:
        if (inst->num_operands >= 3) {
            fprintf(out, "i1 ");
            print_operand(&inst->operands[0], m, out);
            fprintf(out, ", ");
            print_operand(&inst->operands[1], m, out);
            fprintf(out, ", ");
            print_operand(&inst->operands[2], m, out);
        }
        break;

    case LR_OP_STORE:
        if (inst->num_operands >= 2) {
            if (inst->operands[0].type)
                print_type(inst->operands[0].type, out);
            fprintf(out, " ");
            print_operand(&inst->operands[0], m, out);
            fprintf(out, ", ptr ");
            print_operand(&inst->operands[1], m, out);
        }
        break;

    case LR_OP_LOAD:
        if (inst->type) print_type(inst->type, out);
        if (inst->num_operands > 0) {
            fprintf(out, ", ptr ");
            print_operand(&inst->operands[0], m, out);
        }
        break;

    case LR_OP_CALL:
        print_type(inst->type, out);
        fprintf(out, " ");
        if (inst->num_operands > 0) {
            print_operand(&inst->operands[0], m, out);
            fprintf(out, "(");
            for (uint32_t i = 1; i < inst->num_operands; i++) {
                if (i > 1) fprintf(out, ", ");
                if (inst->operands[i].type) {
                    print_type(inst->operands[i].type, out);
                    fprintf(out, " ");
                }
                print_operand(&inst->operands[i], m, out);
            }
            fprintf(out, ")");
        }
        break;

    case LR_OP_ICMP:
        fprintf(out, "%s ", icmp_pred_name(inst->icmp_pred));
        if (inst->num_operands > 0 && inst->operands[0].type)
            print_type(inst->operands[0].type, out);
        fprintf(out, " ");
        for (uint32_t i = 0; i < inst->num_operands; i++) {
            if (i > 0) fprintf(out, ", ");
            print_operand(&inst->operands[i], m, out);
        }
        break;

    case LR_OP_FCMP:
        fprintf(out, "%s ", fcmp_pred_name(inst->fcmp_pred));
        if (inst->num_operands > 0 && inst->operands[0].type)
            print_type(inst->operands[0].type, out);
        fprintf(out, " ");
        for (uint32_t i = 0; i < inst->num_operands; i++) {
            if (i > 0) fprintf(out, ", ");
            print_operand(&inst->operands[i], m, out);
        }
        break;

    case LR_OP_GEP:
        if (inst->type) print_type(inst->type, out);
        for (uint32_t i = 0; i < inst->num_operands; i++) {
            fprintf(out, ", ");
            if (i == 0)
                fprintf(out, "ptr ");
            else if (inst->operands[i].type) {
                print_type(inst->operands[i].type, out);
                fprintf(out, " ");
            }
            print_operand(&inst->operands[i], m, out);
        }
        break;

    default:
        if (is_cast_op(inst->op)) {
            if (inst->num_operands > 0 && inst->operands[0].type) {
                print_type(inst->operands[0].type, out);
                fprintf(out, " ");
                print_operand(&inst->operands[0], m, out);
            }
            fprintf(out, " to ");
            if (inst->type) print_type(inst->type, out);
        } else {
            if (inst->type) {
                print_type(inst->type, out);
                fprintf(out, " ");
            }
            for (uint32_t i = 0; i < inst->num_operands; i++) {
                if (i > 0) fprintf(out, ", ");
                print_operand(&inst->operands[i], m, out);
            }
        }
        break;
    }
    fprintf(out, "\n");
}

/* ---- Module merge ------------------------------------------------------ */

static lr_func_t *merge_find_func(lr_module_t *m, const char *name) {
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        if (f->name && strcmp(f->name, name) == 0)
            return f;
    }
    return NULL;
}

static lr_global_t *merge_find_global(lr_module_t *m, const char *name) {
    for (lr_global_t *g = m->first_global; g; g = g->next) {
        if (g->name && strcmp(g->name, name) == 0)
            return g;
    }
    return NULL;
}

static lr_type_t *merge_remap_type(lr_module_t *dest, const lr_type_t *t) {
    if (!t)
        return NULL;
    switch (t->kind) {
    case LR_TYPE_VOID:   return dest->type_void;
    case LR_TYPE_I1:     return dest->type_i1;
    case LR_TYPE_I8:     return dest->type_i8;
    case LR_TYPE_I16:    return dest->type_i16;
    case LR_TYPE_I32:    return dest->type_i32;
    case LR_TYPE_I64:    return dest->type_i64;
    case LR_TYPE_FLOAT:  return dest->type_float;
    case LR_TYPE_DOUBLE: return dest->type_double;
    case LR_TYPE_PTR:    return dest->type_ptr;
    case LR_TYPE_ARRAY:
        return lr_type_array(dest->arena,
                             merge_remap_type(dest, t->array.elem),
                             t->array.count);
    case LR_TYPE_STRUCT: {
        lr_type_t **fields = NULL;
        char *name = NULL;
        if (t->struc.num_fields > 0) {
            fields = lr_arena_array(dest->arena, lr_type_t *,
                                    t->struc.num_fields);
            for (uint32_t i = 0; i < t->struc.num_fields; i++)
                fields[i] = merge_remap_type(dest, t->struc.fields[i]);
        }
        if (t->struc.name)
            name = lr_arena_strdup(dest->arena, t->struc.name,
                                   strlen(t->struc.name));
        return lr_type_struct(dest->arena, fields, t->struc.num_fields,
                              t->struc.packed, name);
    }
    case LR_TYPE_FUNC: {
        lr_type_t *ret = merge_remap_type(dest, t->func.ret);
        lr_type_t **params = NULL;
        if (t->func.num_params > 0) {
            params = lr_arena_array(dest->arena, lr_type_t *,
                                    t->func.num_params);
            for (uint32_t i = 0; i < t->func.num_params; i++)
                params[i] = merge_remap_type(dest, t->func.params[i]);
        }
        return lr_type_func(dest->arena, ret, params, t->func.num_params,
                             t->func.vararg);
    }
    }
    return NULL;
}

static lr_operand_t merge_remap_operand(lr_module_t *dest,
                                         const lr_operand_t *op,
                                         const uint32_t *symbol_remap) {
    lr_operand_t out = *op;
    out.type = merge_remap_type(dest, op->type);
    if (op->kind == LR_VAL_GLOBAL && symbol_remap)
        out.global_id = symbol_remap[op->global_id];
    return out;
}

static void merge_deep_copy_func_body(lr_module_t *dest, lr_func_t *df,
                                       const lr_func_t *sf,
                                       const uint32_t *symbol_remap) {
    lr_arena_t *a = dest->arena;

    df->next_vreg = sf->next_vreg;

    for (lr_block_t *sb = sf->first_block; sb; sb = sb->next) {
        lr_block_t *db = lr_arena_new(a, lr_block_t);
        db->name = lr_arena_strdup(a, sb->name, strlen(sb->name));
        db->id = sb->id;
        db->func = df;

        for (lr_inst_t *si = sb->first; si; si = si->next) {
            lr_operand_t *ops = NULL;
            if (si->num_operands > 0) {
                ops = lr_arena_array(a, lr_operand_t, si->num_operands);
                for (uint32_t i = 0; i < si->num_operands; i++)
                    ops[i] = merge_remap_operand(dest, &si->operands[i],
                                                 symbol_remap);
            }

            lr_type_t *itype = merge_remap_type(dest, si->type);
            lr_inst_t *di = lr_inst_create(a, si->op, itype, si->dest,
                                           ops, si->num_operands);
            di->icmp_pred = si->icmp_pred;
            di->num_indices = si->num_indices;
            di->call_external_abi = si->call_external_abi;
            di->call_vararg = si->call_vararg;
            di->call_fixed_args = si->call_fixed_args;

            if (si->num_indices > 0 && si->indices) {
                di->indices = lr_arena_array(a, uint32_t, si->num_indices);
                memcpy(di->indices, si->indices,
                       sizeof(uint32_t) * si->num_indices);
            }

            if (!db->first) db->first = di;
            else db->last->next = di;
            db->last = di;
        }

        df->num_blocks++;
        if (!df->first_block) df->first_block = db;
        else df->last_block->next = db;
        df->last_block = db;
        df->is_decl = false;
    }
}

static void merge_replace_func(lr_module_t *dest, lr_func_t *df,
                                const lr_func_t *sf,
                                const uint32_t *symbol_remap) {
    lr_arena_t *a = dest->arena;

    df->ret_type = merge_remap_type(dest, sf->ret_type);
    df->num_params = sf->num_params;
    df->vararg = sf->vararg;

    if (sf->num_params > 0) {
        df->param_types = lr_arena_array(a, lr_type_t *, sf->num_params);
        df->param_vregs = lr_arena_array(a, uint32_t, sf->num_params);
        for (uint32_t i = 0; i < sf->num_params; i++) {
            df->param_types[i] = merge_remap_type(dest, sf->param_types[i]);
            df->param_vregs[i] = sf->param_vregs[i];
        }
    }

    df->type = lr_type_func(a, df->ret_type, df->param_types,
                             df->num_params, df->vararg);

    df->first_block = NULL;
    df->last_block = NULL;
    df->block_array = NULL;
    df->linear_inst_array = NULL;
    df->block_inst_offsets = NULL;
    df->num_linear_insts = 0;
    df->num_blocks = 0;

    merge_deep_copy_func_body(dest, df, sf, symbol_remap);
}

static void merge_copy_global_data(lr_module_t *dest, lr_global_t *dg,
                                    const lr_global_t *sg) {
    lr_arena_t *a = dest->arena;
    if (sg->init_data && sg->init_size > 0) {
        dg->init_data = lr_arena_alloc(a, sg->init_size, 1);
        memcpy(dg->init_data, sg->init_data, sg->init_size);
        dg->init_size = sg->init_size;
    }
    for (lr_reloc_t *sr = sg->relocs; sr; sr = sr->next) {
        lr_reloc_t *dr = lr_arena_new(a, lr_reloc_t);
        dr->offset = sr->offset;
        dr->addend = sr->addend;
        dr->symbol_name = lr_arena_strdup(a, sr->symbol_name,
                                          strlen(sr->symbol_name));
        dr->next = dg->relocs;
        dg->relocs = dr;
    }
}

int lr_module_merge(lr_module_t *dest, lr_module_t *src) {
    uint32_t *symbol_remap = NULL;
    lr_arena_t *a = dest->arena;

    if (!dest || !src)
        return -1;

    if (src->num_symbols > 0) {
        symbol_remap = lr_arena_array(a, uint32_t, src->num_symbols);
        for (uint32_t i = 0; i < src->num_symbols; i++) {
            const char *name = src->symbol_names[i];
            symbol_remap[i] = lr_module_intern_symbol(dest, name);
        }
    }

    for (lr_global_t *sg = src->first_global; sg; sg = sg->next) {
        lr_global_t *dg = merge_find_global(dest, sg->name);

        if (dg) {
            if (dg->is_external && !sg->is_external) {
                dg->type = merge_remap_type(dest, sg->type);
                dg->is_const = sg->is_const;
                dg->is_external = false;
                merge_copy_global_data(dest, dg, sg);
            }
        } else {
            lr_global_t *ng = lr_global_create(dest, sg->name,
                merge_remap_type(dest, sg->type), sg->is_const);
            ng->is_external = sg->is_external;
            merge_copy_global_data(dest, ng, sg);
        }
    }

    for (lr_func_t *sf = src->first_func; sf; sf = sf->next) {
        bool src_is_decl = sf->is_decl || !sf->first_block;
        lr_func_t *df = merge_find_func(dest, sf->name);

        if (df) {
            bool dest_is_decl = df->is_decl || !df->first_block;
            if (dest_is_decl && !src_is_decl)
                merge_replace_func(dest, df, sf, symbol_remap);
        } else {
            lr_type_t **params = NULL;
            if (sf->num_params > 0) {
                params = lr_arena_array(a, lr_type_t *, sf->num_params);
                for (uint32_t i = 0; i < sf->num_params; i++)
                    params[i] = merge_remap_type(dest, sf->param_types[i]);
            }

            if (src_is_decl) {
                lr_func_declare(dest, sf->name,
                    merge_remap_type(dest, sf->ret_type),
                    params, sf->num_params, sf->vararg);
            } else {
                lr_func_t *nf = lr_func_create(dest, sf->name,
                    merge_remap_type(dest, sf->ret_type),
                    params, sf->num_params, sf->vararg);
                nf->first_block = NULL;
                nf->last_block = NULL;
                nf->block_array = NULL;
                nf->linear_inst_array = NULL;
                nf->block_inst_offsets = NULL;
                nf->num_linear_insts = 0;
                nf->num_blocks = 0;
                merge_deep_copy_func_body(dest, nf, sf, symbol_remap);
            }
        }
    }

    return 0;
}

void lr_module_dump(lr_module_t *m, FILE *out) {
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        bool is_decl = f->is_decl || !f->first_block;
        fprintf(out, "%s ", is_decl ? "declare" : "define");
        print_type(f->ret_type, out);
        fprintf(out, " @%s(", f->name);
        for (uint32_t i = 0; i < f->num_params; i++) {
            if (i > 0) fprintf(out, ", ");
            print_type(f->param_types[i], out);
            if (!is_decl) fprintf(out, " %%v%u", f->param_vregs[i]);
        }
        if (f->vararg) {
            if (f->num_params > 0) fprintf(out, ", ");
            fprintf(out, "...");
        }
        fprintf(out, ")");
        if (is_decl) { fprintf(out, "\n\n"); continue; }
        fprintf(out, " {\n");
        for (lr_block_t *b = f->first_block; b; b = b->next) {
            fprintf(out, "%s:\n", b->name);
            for (lr_inst_t *inst = b->first; inst; inst = inst->next)
                dump_inst(inst, m, out);
        }
        fprintf(out, "}\n");
    }
}
