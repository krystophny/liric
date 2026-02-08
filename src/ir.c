#include "ir.h"
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

lr_inst_t *lr_inst_create(lr_arena_t *a, lr_opcode_t op, lr_type_t *type,
                           uint32_t dest, lr_operand_t *ops, uint32_t nops) {
    lr_inst_t *inst = lr_arena_new(a, lr_inst_t);
    inst->op = op;
    inst->type = type;
    inst->dest = dest;
    if (nops > 0) {
        inst->operands = lr_arena_array(a, lr_operand_t, nops);
        memcpy(inst->operands, ops, sizeof(lr_operand_t) * nops);
    }
    inst->num_operands = nops;
    return inst;
}

void lr_block_append(lr_block_t *b, lr_inst_t *inst) {
    if (!b->first) b->first = inst;
    else b->last->next = inst;
    b->last = inst;
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

lr_phi_copy_t **lr_build_phi_copies(lr_arena_t *arena, lr_func_t *func) {
    lr_phi_copy_t **copies = lr_arena_array(arena, lr_phi_copy_t *, func->num_blocks);
    for (uint32_t i = 0; i < func->num_blocks; i++)
        copies[i] = NULL;

    for (lr_block_t *b = func->first_block; b; b = b->next) {
        for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
            if (inst->op != LR_OP_PHI)
                continue;
            for (uint32_t i = 0; i + 1 < inst->num_operands; i += 2) {
                uint32_t pred_id = inst->operands[i + 1].block_id;
                if (pred_id >= func->num_blocks)
                    continue;
                lr_phi_copy_t *pc = lr_arena_new(arena, lr_phi_copy_t);
                pc->dest_vreg = inst->dest;
                pc->src_op = inst->operands[i];
                pc->next = copies[pred_id];
                copies[pred_id] = pc;
            }
        }
    }
    return copies;
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
    case LR_OP_FPTOSI:       return "fptosi";
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
           op == LR_OP_INTTOPTR || op == LR_OP_SITOFP ||
           op == LR_OP_FPTOSI || op == LR_OP_FPEXT || op == LR_OP_FPTRUNC;
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
