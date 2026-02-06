#include "ir.h"
#include <string.h>

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
    if (!f->first_block) f->first_block = b;
    else f->last_block->next = b;
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
    return (lr_operand_t){ .kind = LR_VAL_VREG, .vreg = vreg, .type = type };
}

lr_operand_t lr_op_imm_i64(int64_t val, lr_type_t *type) {
    return (lr_operand_t){ .kind = LR_VAL_IMM_I64, .imm_i64 = val, .type = type };
}

lr_operand_t lr_op_imm_f64(double val, lr_type_t *type) {
    return (lr_operand_t){ .kind = LR_VAL_IMM_F64, .imm_f64 = val, .type = type };
}

lr_operand_t lr_op_block(uint32_t id) {
    return (lr_operand_t){ .kind = LR_VAL_BLOCK, .block_id = id };
}

lr_operand_t lr_op_global(uint32_t id, lr_type_t *type) {
    return (lr_operand_t){ .kind = LR_VAL_GLOBAL, .global_id = id, .type = type };
}

lr_operand_t lr_op_null(lr_type_t *type) {
    return (lr_operand_t){ .kind = LR_VAL_NULL, .type = type };
}

uint32_t lr_module_intern_symbol(lr_module_t *m, const char *name) {
    for (uint32_t i = 0; i < m->num_symbols; i++) {
        if (strcmp(m->symbol_names[i], name) == 0)
            return i;
    }

    if (m->num_symbols == m->symbol_cap) {
        uint32_t old_cap = m->symbol_cap;
        uint32_t new_cap = old_cap == 0 ? 64 : old_cap * 2;
        char **names = lr_arena_array(m->arena, char *, new_cap);
        if (old_cap > 0)
            memcpy(names, m->symbol_names, sizeof(char *) * old_cap);
        m->symbol_names = names;
        m->symbol_cap = new_cap;
    }

    uint32_t id = m->num_symbols++;
    m->symbol_names[id] = lr_arena_strdup(m->arena, name, strlen(name));
    return id;
}

const char *lr_module_symbol_name(const lr_module_t *m, uint32_t id) {
    if (!m || id >= m->num_symbols)
        return NULL;
    return m->symbol_names[id];
}

size_t lr_type_size(const lr_type_t *t) {
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

static void print_operand(const lr_operand_t *op, FILE *out) {
    switch (op->kind) {
    case LR_VAL_VREG:    fprintf(out, "%%v%u", op->vreg); break;
    case LR_VAL_IMM_I64: fprintf(out, "%ld", (long)op->imm_i64); break;
    case LR_VAL_IMM_F64: fprintf(out, "%g", op->imm_f64); break;
    case LR_VAL_BLOCK:   fprintf(out, "label %%bb%u", op->block_id); break;
    case LR_VAL_GLOBAL:  fprintf(out, "@g%u", op->global_id); break;
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

void lr_module_dump(lr_module_t *m, FILE *out) {
    for (lr_func_t *f = m->first_func; f; f = f->next) {
        fprintf(out, "%s ", f->is_decl ? "declare" : "define");
        print_type(f->ret_type, out);
        fprintf(out, " @%s(", f->name);
        for (uint32_t i = 0; i < f->num_params; i++) {
            if (i > 0) fprintf(out, ", ");
            print_type(f->param_types[i], out);
            if (!f->is_decl) fprintf(out, " %%v%u", f->param_vregs[i]);
        }
        if (f->vararg) {
            if (f->num_params > 0) fprintf(out, ", ");
            fprintf(out, "...");
        }
        fprintf(out, ")");
        if (f->is_decl) { fprintf(out, "\n"); continue; }
        fprintf(out, " {\n");
        for (lr_block_t *b = f->first_block; b; b = b->next) {
            fprintf(out, "%s:\n", b->name);
            for (lr_inst_t *inst = b->first; inst; inst = inst->next) {
                fprintf(out, "  ");
                if (inst->op != LR_OP_RET && inst->op != LR_OP_RET_VOID &&
                    inst->op != LR_OP_BR && inst->op != LR_OP_CONDBR &&
                    inst->op != LR_OP_STORE && inst->op != LR_OP_UNREACHABLE) {
                    fprintf(out, "%%v%u = ", inst->dest);
                }
                fprintf(out, "%s ", opcode_name(inst->op));
                if (inst->type && inst->op != LR_OP_RET_VOID) {
                    print_type(inst->type, out);
                    fprintf(out, " ");
                }
                for (uint32_t i = 0; i < inst->num_operands; i++) {
                    if (i > 0) fprintf(out, ", ");
                    print_operand(&inst->operands[i], out);
                }
                fprintf(out, "\n");
            }
        }
        fprintf(out, "}\n");
    }
}
