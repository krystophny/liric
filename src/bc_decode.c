#include "bc_decode.h"
#include "frontend_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LIRIC_HAVE_LLVM_BITCODE
#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>
#endif

bool lr_bc_is_bitcode(const uint8_t *data, size_t len) {
    if (!data || len < 4)
        return false;

    /* Raw LLVM bitcode magic: "BC\\xC0\\xDE". */
    if (data[0] == 0x42 && data[1] == 0x43 && data[2] == 0xC0 && data[3] == 0xDE)
        return true;

    /* LLVM bitcode wrapper magic (little-endian 0x0B17C0DE). */
    if (data[0] == 0xDE && data[1] == 0xC0 && data[2] == 0x17 && data[3] == 0x0B)
        return true;

    return false;
}

#if !LIRIC_HAVE_LLVM_BITCODE

bool lr_bc_parser_available(void) {
    return false;
}

lr_module_t *lr_parse_bc_data(const uint8_t *data, size_t len,
                              lr_arena_t *arena, char *err, size_t errlen) {
    (void)arena;

    if (!data && len != 0) {
        lr_frontend_set_error(err, errlen, "invalid bitcode input buffer");
        return NULL;
    }

    if (!lr_bc_is_bitcode(data, len)) {
        lr_frontend_set_error(err, errlen, "input is not LLVM bitcode");
        return NULL;
    }

    lr_frontend_set_error(err, errlen,
                          "LLVM bitcode (.bc) detected, but this build has no LLVM bitcode decoder support");
    return NULL;
}

#else

typedef struct {
    LLVMTypeRef ll;
    lr_type_t *lr;
} bc_type_entry_t;

typedef struct {
    LLVMValueRef ll_value;
    uint32_t vreg;
    lr_type_t *type;
} bc_value_entry_t;

typedef struct {
    LLVMBasicBlockRef ll_block;
    lr_block_t *lr_block;
} bc_block_entry_t;

typedef struct {
    bc_value_entry_t *entries;
    uint32_t count;
    uint32_t cap;
} bc_value_map_t;

typedef struct {
    bc_block_entry_t *entries;
    uint32_t count;
    uint32_t cap;
} bc_block_map_t;

typedef struct {
    bc_type_entry_t *entries;
    uint32_t count;
    uint32_t cap;
    lr_module_t *module;
    lr_arena_t *arena;
    char *err;
    size_t errlen;
} bc_ctx_t;

typedef struct {
    bc_ctx_t *bc;
    lr_func_t *func;
    bc_value_map_t values;
    bc_block_map_t blocks;
} bc_func_ctx_t;

static void bc_set_error(bc_ctx_t *bc, const char *fmt, ...) {
    va_list ap;
    if (!bc || !bc->err || bc->errlen == 0)
        return;
    va_start(ap, fmt);
    vsnprintf(bc->err, bc->errlen, fmt, ap);
    va_end(ap);
}

static bool value_map_put(bc_value_map_t *map, LLVMValueRef ll_value,
                          uint32_t vreg, lr_type_t *type) {
    bc_value_entry_t *next;
    uint32_t new_cap;

    if (!map)
        return false;

    if (map->count == map->cap) {
        new_cap = map->cap ? map->cap * 2u : 64u;
        next = (bc_value_entry_t *)realloc(map->entries,
                                           (size_t)new_cap * sizeof(*next));
        if (!next)
            return false;
        map->entries = next;
        map->cap = new_cap;
    }

    map->entries[map->count].ll_value = ll_value;
    map->entries[map->count].vreg = vreg;
    map->entries[map->count].type = type;
    map->count++;
    return true;
}

static bool block_map_put(bc_block_map_t *map, LLVMBasicBlockRef ll_block,
                          lr_block_t *lr_block) {
    bc_block_entry_t *next;
    uint32_t new_cap;

    if (!map)
        return false;

    if (map->count == map->cap) {
        new_cap = map->cap ? map->cap * 2u : 32u;
        next = (bc_block_entry_t *)realloc(map->entries,
                                           (size_t)new_cap * sizeof(*next));
        if (!next)
            return false;
        map->entries = next;
        map->cap = new_cap;
    }

    map->entries[map->count].ll_block = ll_block;
    map->entries[map->count].lr_block = lr_block;
    map->count++;
    return true;
}

static bool type_map_put(bc_ctx_t *bc, LLVMTypeRef ll, lr_type_t *lr) {
    bc_type_entry_t *next;
    uint32_t new_cap;

    if (!bc)
        return false;

    if (bc->count == bc->cap) {
        new_cap = bc->cap ? bc->cap * 2u : 64u;
        next = (bc_type_entry_t *)realloc(bc->entries, (size_t)new_cap * sizeof(*next));
        if (!next)
            return false;
        bc->entries = next;
        bc->cap = new_cap;
    }

    bc->entries[bc->count].ll = ll;
    bc->entries[bc->count].lr = lr;
    bc->count++;
    return true;
}

static lr_type_t *type_map_get(bc_ctx_t *bc, LLVMTypeRef ll) {
    uint32_t i;
    if (!bc || !ll)
        return NULL;
    for (i = 0; i < bc->count; i++) {
        if (bc->entries[i].ll == ll)
            return bc->entries[i].lr;
    }
    return NULL;
}

static bc_value_entry_t *value_map_get(bc_value_map_t *map, LLVMValueRef ll_value) {
    uint32_t i;
    if (!map || !ll_value)
        return NULL;
    for (i = 0; i < map->count; i++) {
        if (map->entries[i].ll_value == ll_value)
            return &map->entries[i];
    }
    return NULL;
}

static lr_block_t *block_map_get(bc_block_map_t *map, LLVMBasicBlockRef ll_block) {
    uint32_t i;
    if (!map || !ll_block)
        return NULL;
    for (i = 0; i < map->count; i++) {
        if (map->entries[i].ll_block == ll_block)
            return map->entries[i].lr_block;
    }
    return NULL;
}

static lr_type_t *bc_convert_type(bc_ctx_t *bc, LLVMTypeRef ll_ty) {
    LLVMTypeKind kind;
    lr_type_t *cached;

    if (!bc || !ll_ty)
        return NULL;

    cached = type_map_get(bc, ll_ty);
    if (cached)
        return cached;

    kind = LLVMGetTypeKind(ll_ty);
    switch (kind) {
    case LLVMVoidTypeKind:
        return bc->module->type_void;
    case LLVMIntegerTypeKind: {
        unsigned bits = LLVMGetIntTypeWidth(ll_ty);
        switch (bits) {
        case 1:  return bc->module->type_i1;
        case 8:  return bc->module->type_i8;
        case 16: return bc->module->type_i16;
        case 32: return bc->module->type_i32;
        case 64: return bc->module->type_i64;
        default:
            bc_set_error(bc, "unsupported integer width in bitcode: i%u", bits);
            return NULL;
        }
    }
    case LLVMFloatTypeKind:
        return bc->module->type_float;
    case LLVMDoubleTypeKind:
        return bc->module->type_double;
    case LLVMPointerTypeKind:
        return bc->module->type_ptr;
    case LLVMArrayTypeKind: {
        lr_type_t *elem = bc_convert_type(bc, LLVMGetElementType(ll_ty));
        uint64_t n = LLVMGetArrayLength2(ll_ty);
        lr_type_t *arr;
        if (!elem)
            return NULL;
        arr = lr_type_array(bc->arena, elem, n);
        if (!arr) {
            bc_set_error(bc, "failed to allocate array type");
            return NULL;
        }
        if (!type_map_put(bc, ll_ty, arr)) {
            bc_set_error(bc, "out of memory while caching array type");
            return NULL;
        }
        return arr;
    }
    case LLVMStructTypeKind: {
        lr_type_t *st = lr_arena_new(bc->arena, lr_type_t);
        LLVMTypeRef *ll_fields = NULL;
        lr_type_t **fields = NULL;
        unsigned nfields = LLVMCountStructElementTypes(ll_ty);
        unsigned i;
        const char *name = LLVMGetStructName(ll_ty);
        char *owned_name = NULL;

        if (LLVMIsOpaqueStruct(ll_ty)) {
            bc_set_error(bc, "opaque struct types are not supported in bitcode");
            return NULL;
        }

        if (!st) {
            bc_set_error(bc, "failed to allocate struct type");
            return NULL;
        }
        st->kind = LR_TYPE_STRUCT;
        st->struc.fields = NULL;
        st->struc.num_fields = nfields;
        st->struc.packed = LLVMIsPackedStruct(ll_ty) != 0;
        st->struc.name = NULL;

        if (!type_map_put(bc, ll_ty, st)) {
            bc_set_error(bc, "out of memory while caching struct type");
            return NULL;
        }

        if (name && name[0] != '\0')
            owned_name = lr_arena_strdup(bc->arena, name, strlen(name));
        st->struc.name = owned_name;

        if (nfields == 0)
            return st;

        ll_fields = (LLVMTypeRef *)malloc((size_t)nfields * sizeof(*ll_fields));
        fields = lr_arena_array(bc->arena, lr_type_t *, nfields);
        if (!ll_fields || !fields) {
            free(ll_fields);
            bc_set_error(bc, "out of memory while decoding struct fields");
            return NULL;
        }

        LLVMGetStructElementTypes(ll_ty, ll_fields);
        for (i = 0; i < nfields; i++) {
            fields[i] = bc_convert_type(bc, ll_fields[i]);
            if (!fields[i]) {
                free(ll_fields);
                return NULL;
            }
        }
        free(ll_fields);
        st->struc.fields = fields;
        return st;
    }
    case LLVMFunctionTypeKind: {
        lr_type_t *fn = lr_arena_new(bc->arena, lr_type_t);
        lr_type_t *ret = bc_convert_type(bc, LLVMGetReturnType(ll_ty));
        unsigned nparams = LLVMCountParamTypes(ll_ty);
        LLVMTypeRef *ll_params = NULL;
        lr_type_t **params = NULL;
        unsigned i;

        if (!fn || !ret) {
            bc_set_error(bc, "failed to allocate function type");
            return NULL;
        }
        fn->kind = LR_TYPE_FUNC;
        fn->func.ret = ret;
        fn->func.params = NULL;
        fn->func.num_params = nparams;
        fn->func.vararg = LLVMIsFunctionVarArg(ll_ty) != 0;

        if (!type_map_put(bc, ll_ty, fn)) {
            bc_set_error(bc, "out of memory while caching function type");
            return NULL;
        }

        if (nparams == 0)
            return fn;

        ll_params = (LLVMTypeRef *)malloc((size_t)nparams * sizeof(*ll_params));
        params = lr_arena_array(bc->arena, lr_type_t *, nparams);
        if (!ll_params || !params) {
            free(ll_params);
            bc_set_error(bc, "out of memory while decoding function parameters");
            return NULL;
        }
        LLVMGetParamTypes(ll_ty, ll_params);
        for (i = 0; i < nparams; i++) {
            params[i] = bc_convert_type(bc, ll_params[i]);
            if (!params[i]) {
                free(ll_params);
                return NULL;
            }
        }
        free(ll_params);
        fn->func.params = params;
        return fn;
    }
    default:
        break;
    }

    bc_set_error(bc, "unsupported LLVM type kind in bitcode: %d", (int)kind);
    return NULL;
}

static bool bc_map_icmp_pred(LLVMIntPredicate pred, lr_icmp_pred_t *out) {
    if (!out)
        return false;
    switch (pred) {
    case LLVMIntEQ:  *out = LR_ICMP_EQ; return true;
    case LLVMIntNE:  *out = LR_ICMP_NE; return true;
    case LLVMIntSGT: *out = LR_ICMP_SGT; return true;
    case LLVMIntSGE: *out = LR_ICMP_SGE; return true;
    case LLVMIntSLT: *out = LR_ICMP_SLT; return true;
    case LLVMIntSLE: *out = LR_ICMP_SLE; return true;
    case LLVMIntUGT: *out = LR_ICMP_UGT; return true;
    case LLVMIntUGE: *out = LR_ICMP_UGE; return true;
    case LLVMIntULT: *out = LR_ICMP_ULT; return true;
    case LLVMIntULE: *out = LR_ICMP_ULE; return true;
    default:
        return false;
    }
}

static bool bc_map_fcmp_pred(LLVMRealPredicate pred, lr_fcmp_pred_t *out) {
    if (!out)
        return false;
    switch (pred) {
    case LLVMRealPredicateFalse: *out = LR_FCMP_FALSE; return true;
    case LLVMRealOEQ: *out = LR_FCMP_OEQ; return true;
    case LLVMRealOGT: *out = LR_FCMP_OGT; return true;
    case LLVMRealOGE: *out = LR_FCMP_OGE; return true;
    case LLVMRealOLT: *out = LR_FCMP_OLT; return true;
    case LLVMRealOLE: *out = LR_FCMP_OLE; return true;
    case LLVMRealONE: *out = LR_FCMP_ONE; return true;
    case LLVMRealORD: *out = LR_FCMP_ORD; return true;
    case LLVMRealUEQ: *out = LR_FCMP_UEQ; return true;
    case LLVMRealUGT: *out = LR_FCMP_UGT; return true;
    case LLVMRealUGE: *out = LR_FCMP_UGE; return true;
    case LLVMRealULT: *out = LR_FCMP_ULT; return true;
    case LLVMRealULE: *out = LR_FCMP_ULE; return true;
    case LLVMRealUNE: *out = LR_FCMP_UNE; return true;
    case LLVMRealUNO: *out = LR_FCMP_UNO; return true;
    case LLVMRealPredicateTrue: *out = LR_FCMP_TRUE; return true;
    default:
        return false;
    }
}

static bool bc_map_binary_opcode(LLVMOpcode opcode, lr_opcode_t *out) {
    if (!out)
        return false;
    switch (opcode) {
    case LLVMAdd:  *out = LR_OP_ADD;  return true;
    case LLVMSub:  *out = LR_OP_SUB;  return true;
    case LLVMMul:  *out = LR_OP_MUL;  return true;
    case LLVMSDiv: *out = LR_OP_SDIV; return true;
    case LLVMUDiv: *out = LR_OP_SDIV; return true;
    case LLVMSRem: *out = LR_OP_SREM; return true;
    case LLVMURem: *out = LR_OP_SREM; return true;
    case LLVMAnd:  *out = LR_OP_AND;  return true;
    case LLVMOr:   *out = LR_OP_OR;   return true;
    case LLVMXor:  *out = LR_OP_XOR;  return true;
    case LLVMShl:  *out = LR_OP_SHL;  return true;
    case LLVMLShr: *out = LR_OP_LSHR; return true;
    case LLVMAShr: *out = LR_OP_ASHR; return true;
    case LLVMFAdd: *out = LR_OP_FADD; return true;
    case LLVMFSub: *out = LR_OP_FSUB; return true;
    case LLVMFMul: *out = LR_OP_FMUL; return true;
    case LLVMFDiv: *out = LR_OP_FDIV; return true;
    default:
        return false;
    }
}

static bool bc_value_name(LLVMValueRef value, const char **out_name, size_t *out_len) {
    const char *name;
    size_t len;
    if (!value || !out_name || !out_len)
        return false;
    name = LLVMGetValueName2(value, &len);
    if (!name || len == 0)
        return false;
    *out_name = name;
    *out_len = len;
    return true;
}

static bool bc_make_operand(bc_ctx_t *bc, bc_func_ctx_t *fnc, LLVMValueRef ll_value,
                            lr_type_t *expected_type, lr_operand_t *out) {
    lr_type_t *value_type;
    bc_value_entry_t *mapped;
    const char *name = NULL;
    size_t name_len = 0;
    uint32_t sym_id;

    if (!bc || !fnc || !ll_value || !out)
        return false;

    mapped = value_map_get(&fnc->values, ll_value);
    if (mapped) {
        *out = lr_op_vreg(mapped->vreg, mapped->type);
        return true;
    }

    value_type = bc_convert_type(bc, LLVMTypeOf(ll_value));
    if (!value_type)
        return false;

    if (LLVMIsAConstantInt(ll_value)) {
        *out = lr_op_imm_i64((int64_t)LLVMConstIntGetSExtValue(ll_value), value_type);
        return true;
    }

    if (LLVMIsAConstantFP(ll_value)) {
        LLVMBool loses_info = 0;
        *out = lr_op_imm_f64(LLVMConstRealGetDouble(ll_value, &loses_info), value_type);
        return true;
    }

    if (LLVMIsAConstantPointerNull(ll_value)) {
        *out = lr_op_null(expected_type ? expected_type : bc->module->type_ptr);
        return true;
    }

    if (LLVMIsAUndefValue(ll_value) || LLVMIsAPoisonValue(ll_value)) {
        out->kind = LR_VAL_UNDEF;
        out->type = expected_type ? expected_type : value_type;
        out->global_offset = 0;
        return true;
    }

    if (LLVMIsABasicBlock(ll_value)) {
        LLVMBasicBlockRef bb = LLVMValueAsBasicBlock(ll_value);
        lr_block_t *lr_bb = block_map_get(&fnc->blocks, bb);
        if (!lr_bb) {
            bc_set_error(bc, "failed to resolve basic block operand");
            return false;
        }
        *out = lr_op_block(lr_bb->id);
        return true;
    }

    if (LLVMIsAFunction(ll_value) || LLVMIsAGlobalVariable(ll_value)) {
        if (!bc_value_name(ll_value, &name, &name_len)) {
            bc_set_error(bc, "failed to resolve symbol name for global operand");
            return false;
        }
        {
            char *owned = lr_arena_strdup(bc->arena, name, name_len);
            sym_id = lr_frontend_intern_symbol(bc->module, owned);
            if (sym_id == UINT32_MAX) {
                bc_set_error(bc, "failed to intern symbol '%s'", owned);
                return false;
            }
        }
        *out = lr_op_global(sym_id, expected_type ? expected_type : bc->module->type_ptr);
        return true;
    }

    if (LLVMIsAConstantExpr(ll_value)) {
        LLVMOpcode op = LLVMGetConstOpcode(ll_value);
        if (op == LLVMBitCast || op == LLVMAddrSpaceCast ||
            op == LLVMPtrToInt || op == LLVMIntToPtr) {
            return bc_make_operand(bc, fnc, LLVMGetOperand(ll_value, 0),
                                   expected_type, out);
        }
    }

    bc_set_error(bc, "unsupported LLVM value kind in bitcode operand");
    return false;
}

static bool bc_append_inst(lr_block_t *block, lr_inst_t *inst, bc_ctx_t *bc,
                           const char *what) {
    if (!block || !inst || !bc) {
        if (bc)
            bc_set_error(bc, "internal error appending instruction");
        return false;
    }
    lr_block_append(block, inst);
    (void)what;
    return true;
}

static bool bc_translate_instruction(bc_ctx_t *bc, bc_func_ctx_t *fnc,
                                     lr_block_t *lr_block, LLVMValueRef inst) {
    LLVMOpcode opcode;
    lr_inst_t *lr_inst;
    uint32_t dest = 0;
    lr_type_t *dest_type = NULL;
    bc_value_entry_t *mapped = NULL;

    if (!bc || !fnc || !lr_block || !inst)
        return false;

    mapped = value_map_get(&fnc->values, inst);
    if (mapped) {
        dest = mapped->vreg;
        dest_type = mapped->type;
    }

    opcode = LLVMGetInstructionOpcode(inst);

    if (opcode == LLVMRet) {
        int nops = LLVMGetNumOperands(inst);
        if (nops == 0) {
            lr_inst = lr_inst_create(bc->arena, LR_OP_RET_VOID,
                                     bc->module->type_void, 0, NULL, 0);
        } else {
            lr_operand_t op;
            if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), dest_type, &op))
                return false;
            lr_inst = lr_inst_create(bc->arena, LR_OP_RET, op.type, 0, &op, 1);
        }
        return bc_append_inst(lr_block, lr_inst, bc, "ret");
    }

    if (opcode == LLVMBr) {
        unsigned nsucc = LLVMGetNumSuccessors(inst);
        if (nsucc == 1) {
            lr_block_t *dst = block_map_get(&fnc->blocks, LLVMGetSuccessor(inst, 0));
            lr_operand_t op;
            if (!dst) {
                bc_set_error(bc, "failed to resolve branch successor");
                return false;
            }
            op = lr_op_block(dst->id);
            lr_inst = lr_inst_create(bc->arena, LR_OP_BR, bc->module->type_void, 0, &op, 1);
            return bc_append_inst(lr_block, lr_inst, bc, "br");
        } else if (nsucc == 2) {
            lr_operand_t ops[3];
            lr_block_t *t = block_map_get(&fnc->blocks, LLVMGetSuccessor(inst, 0));
            lr_block_t *f = block_map_get(&fnc->blocks, LLVMGetSuccessor(inst, 1));
            if (!t || !f) {
                bc_set_error(bc, "failed to resolve conditional branch successors");
                return false;
            }
            if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), bc->module->type_i1, &ops[0]))
                return false;
            ops[1] = lr_op_block(t->id);
            ops[2] = lr_op_block(f->id);
            lr_inst = lr_inst_create(bc->arena, LR_OP_CONDBR,
                                     bc->module->type_void, 0, ops, 3);
            return bc_append_inst(lr_block, lr_inst, bc, "condbr");
        }

        bc_set_error(bc, "unsupported branch form with %u successors", nsucc);
        return false;
    }

    if (opcode == LLVMUnreachable) {
        lr_inst = lr_inst_create(bc->arena, LR_OP_UNREACHABLE, bc->module->type_void,
                                 0, NULL, 0);
        return bc_append_inst(lr_block, lr_inst, bc, "unreachable");
    }

    if (opcode == LLVMICmp) {
        lr_icmp_pred_t pred;
        lr_operand_t ops[2];
        if (!bc_map_icmp_pred(LLVMGetICmpPredicate(inst), &pred)) {
            bc_set_error(bc, "unsupported icmp predicate in bitcode");
            return false;
        }
        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), NULL, &ops[0]) ||
            !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 1), ops[0].type, &ops[1]))
            return false;
        lr_inst = lr_inst_create(bc->arena, LR_OP_ICMP, bc->module->type_i1,
                                 dest, ops, 2);
        if (!lr_inst)
            return false;
        lr_inst->icmp_pred = pred;
        return bc_append_inst(lr_block, lr_inst, bc, "icmp");
    }

    if (opcode == LLVMFCmp) {
        lr_fcmp_pred_t pred;
        lr_operand_t ops[2];
        if (!bc_map_fcmp_pred(LLVMGetFCmpPredicate(inst), &pred)) {
            bc_set_error(bc, "unsupported fcmp predicate in bitcode");
            return false;
        }
        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), NULL, &ops[0]) ||
            !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 1), ops[0].type, &ops[1]))
            return false;
        lr_inst = lr_inst_create(bc->arena, LR_OP_FCMP, bc->module->type_i1,
                                 dest, ops, 2);
        if (!lr_inst)
            return false;
        lr_inst->fcmp_pred = pred;
        return bc_append_inst(lr_block, lr_inst, bc, "fcmp");
    }

    if (opcode == LLVMAlloca) {
        int nops = LLVMGetNumOperands(inst);
        lr_operand_t ops_local[1];
        lr_operand_t *ops = NULL;
        lr_type_t *elem_ty = bc_convert_type(bc, LLVMGetAllocatedType(inst));
        if (!elem_ty)
            return false;
        if (nops > 0) {
            if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0),
                                 bc->module->type_i64, &ops_local[0]))
                return false;
            ops = ops_local;
        }
        lr_inst = lr_inst_create(bc->arena, LR_OP_ALLOCA, bc->module->type_ptr,
                                 dest, ops, (uint32_t)(nops > 0 ? 1 : 0));
        if (!lr_inst)
            return false;
        lr_inst->type = elem_ty;
        return bc_append_inst(lr_block, lr_inst, bc, "alloca");
    }

    if (opcode == LLVMLoad) {
        lr_operand_t op;
        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), bc->module->type_ptr, &op))
            return false;
        lr_inst = lr_inst_create(bc->arena, LR_OP_LOAD, dest_type, dest, &op, 1);
        return bc_append_inst(lr_block, lr_inst, bc, "load");
    }

    if (opcode == LLVMStore) {
        lr_operand_t ops[2];
        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), NULL, &ops[0]) ||
            !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 1), bc->module->type_ptr, &ops[1]))
            return false;
        lr_inst = lr_inst_create(bc->arena, LR_OP_STORE, bc->module->type_void, 0, ops, 2);
        return bc_append_inst(lr_block, lr_inst, bc, "store");
    }

    if (opcode == LLVMGetElementPtr) {
        int nraw = LLVMGetNumOperands(inst);
        uint32_t nops = (uint32_t)nraw;
        lr_operand_t *ops;
        lr_type_t *base_ty;
        uint32_t i;
        if (nops == 0) {
            bc_set_error(bc, "invalid gep with zero operands");
            return false;
        }
        ops = (lr_operand_t *)malloc((size_t)nops * sizeof(*ops));
        if (!ops) {
            bc_set_error(bc, "out of memory in gep decode");
            return false;
        }

        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), bc->module->type_ptr, &ops[0])) {
            free(ops);
            return false;
        }
        for (i = 1; i < nops; i++) {
            if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, (unsigned)i), NULL, &ops[i])) {
                free(ops);
                return false;
            }
            ops[i] = lr_canonicalize_gep_index(bc->module, lr_block, fnc->func, ops[i]);
        }

        base_ty = bc_convert_type(bc, LLVMGetGEPSourceElementType(inst));
        if (!base_ty) {
            free(ops);
            return false;
        }

        lr_inst = lr_inst_create(bc->arena, LR_OP_GEP, bc->module->type_ptr, dest, ops, nops);
        free(ops);
        if (!lr_inst)
            return false;
        lr_inst->type = base_ty;
        return bc_append_inst(lr_block, lr_inst, bc, "gep");
    }

    if (opcode == LLVMCall) {
        unsigned nargs = LLVMGetNumArgOperands(inst);
        uint32_t nops = (uint32_t)nargs + 1u;
        lr_operand_t *ops = (lr_operand_t *)malloc((size_t)nops * sizeof(*ops));
        LLVMTypeRef called_fty = LLVMGetCalledFunctionType(inst);
        unsigned nparam_types = called_fty ? LLVMCountParamTypes(called_fty) : 0;
        LLVMTypeRef *param_types = NULL;
        unsigned i;

        if (!ops) {
            bc_set_error(bc, "out of memory in call decode");
            return false;
        }

        if (!bc_make_operand(bc, fnc, LLVMGetCalledValue(inst), bc->module->type_ptr, &ops[0])) {
            free(ops);
            return false;
        }

        if (called_fty && nparam_types > 0) {
            param_types = (LLVMTypeRef *)malloc((size_t)nparam_types * sizeof(*param_types));
            if (!param_types) {
                free(ops);
                bc_set_error(bc, "out of memory in call parameter decode");
                return false;
            }
            LLVMGetParamTypes(called_fty, param_types);
        }

        for (i = 0; i < nargs; i++) {
            lr_type_t *expected = NULL;
            if (called_fty && i < nparam_types)
                expected = bc_convert_type(bc, param_types[i]);
            if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, i), expected, &ops[i + 1])) {
                free(param_types);
                free(ops);
                return false;
            }
        }
        free(param_types);

        if (!dest_type && called_fty)
            dest_type = bc_convert_type(bc, LLVMGetReturnType(called_fty));
        if (!dest_type)
            dest_type = bc->module->type_void;

        lr_inst = lr_inst_create(bc->arena, LR_OP_CALL, dest_type, dest, ops, nops);
        free(ops);
        return bc_append_inst(lr_block, lr_inst, bc, "call");
    }

    if (opcode == LLVMPHI) {
        unsigned nin = LLVMCountIncoming(inst);
        uint32_t nops = (uint32_t)(nin * 2u);
        lr_operand_t *ops = (lr_operand_t *)malloc((size_t)nops * sizeof(*ops));
        unsigned i;
        if (!ops) {
            bc_set_error(bc, "out of memory in phi decode");
            return false;
        }
        for (i = 0; i < nin; i++) {
            LLVMValueRef in_val = LLVMGetIncomingValue(inst, i);
            LLVMBasicBlockRef in_block = LLVMGetIncomingBlock(inst, i);
            lr_block_t *pred = block_map_get(&fnc->blocks, in_block);
            if (!pred) {
                free(ops);
                bc_set_error(bc, "failed to resolve phi predecessor block");
                return false;
            }
            if (!bc_make_operand(bc, fnc, in_val, dest_type, &ops[2u * i])) {
                free(ops);
                return false;
            }
            ops[2u * i + 1u] = lr_op_block(pred->id);
        }
        lr_inst = lr_inst_create(bc->arena, LR_OP_PHI, dest_type, dest, ops, nops);
        free(ops);
        return bc_append_inst(lr_block, lr_inst, bc, "phi");
    }

    if (opcode == LLVMSelect) {
        lr_operand_t ops[3];
        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), bc->module->type_i1, &ops[0]) ||
            !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 1), dest_type, &ops[1]) ||
            !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 2), dest_type, &ops[2]))
            return false;
        lr_inst = lr_inst_create(bc->arena, LR_OP_SELECT, dest_type, dest, ops, 3);
        return bc_append_inst(lr_block, lr_inst, bc, "select");
    }

    if (opcode == LLVMFNeg) {
        lr_operand_t op;
        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), dest_type, &op))
            return false;
        lr_inst = lr_inst_create(bc->arena, LR_OP_FNEG, dest_type, dest, &op, 1);
        return bc_append_inst(lr_block, lr_inst, bc, "fneg");
    }

    if (opcode == LLVMSExt || opcode == LLVMZExt || opcode == LLVMTrunc ||
        opcode == LLVMBitCast || opcode == LLVMPtrToInt || opcode == LLVMIntToPtr ||
        opcode == LLVMUIToFP || opcode == LLVMSIToFP || opcode == LLVMFPToSI ||
        opcode == LLVMFPToUI || opcode == LLVMFPExt || opcode == LLVMFPTrunc) {
        lr_opcode_t cast_op = LR_OP_BITCAST;
        lr_operand_t op;

        switch (opcode) {
        case LLVMSExt:    cast_op = LR_OP_SEXT; break;
        case LLVMZExt:    cast_op = LR_OP_ZEXT; break;
        case LLVMTrunc:   cast_op = LR_OP_TRUNC; break;
        case LLVMBitCast: cast_op = LR_OP_BITCAST; break;
        case LLVMPtrToInt: cast_op = LR_OP_PTRTOINT; break;
        case LLVMIntToPtr: cast_op = LR_OP_INTTOPTR; break;
        case LLVMUIToFP:
        case LLVMSIToFP:  cast_op = LR_OP_SITOFP; break;
        case LLVMFPToSI:
        case LLVMFPToUI:  cast_op = LR_OP_FPTOSI; break;
        case LLVMFPExt:   cast_op = LR_OP_FPEXT; break;
        case LLVMFPTrunc: cast_op = LR_OP_FPTRUNC; break;
        default: break;
        }

        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), NULL, &op))
            return false;
        lr_inst = lr_inst_create(bc->arena, cast_op, dest_type, dest, &op, 1);
        return bc_append_inst(lr_block, lr_inst, bc, "cast");
    }

    if (opcode == LLVMExtractValue || opcode == LLVMInsertValue) {
        uint32_t nops = (opcode == LLVMExtractValue) ? 1u : 2u;
        lr_operand_t ops[2];
        uint32_t nidx = (uint32_t)LLVMGetNumIndices(inst);
        const unsigned *idx_src = LLVMGetIndices(inst);
        uint32_t *idx_copy = NULL;
        uint32_t i;

        if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), NULL, &ops[0]))
            return false;
        if (opcode == LLVMInsertValue &&
            !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 1), NULL, &ops[1]))
            return false;

        lr_inst = lr_inst_create(bc->arena,
                                 opcode == LLVMExtractValue ? LR_OP_EXTRACTVALUE : LR_OP_INSERTVALUE,
                                 dest_type, dest, ops, nops);
        if (!lr_inst)
            return false;

        if (nidx > 0) {
            idx_copy = lr_arena_array(bc->arena, uint32_t, nidx);
            if (!idx_copy) {
                bc_set_error(bc, "out of memory in aggregate index decode");
                return false;
            }
            for (i = 0; i < nidx; i++)
                idx_copy[i] = idx_src[i];
        }
        lr_inst->indices = idx_copy;
        lr_inst->num_indices = nidx;
        return bc_append_inst(lr_block, lr_inst, bc, "aggregate");
    }

    {
        lr_opcode_t bin_op;
        lr_operand_t ops[2];
        if (bc_map_binary_opcode(opcode, &bin_op)) {
            if (!bc_make_operand(bc, fnc, LLVMGetOperand(inst, 0), dest_type, &ops[0]) ||
                !bc_make_operand(bc, fnc, LLVMGetOperand(inst, 1), dest_type, &ops[1]))
                return false;
            lr_inst = lr_inst_create(bc->arena, bin_op, dest_type, dest, ops, 2);
            return bc_append_inst(lr_block, lr_inst, bc, "binary");
        }
    }

    bc_set_error(bc, "unsupported LLVM instruction opcode in bitcode: %d", (int)opcode);
    return false;
}

static bool bc_translate_function_body(bc_ctx_t *bc, LLVMValueRef ll_fn, lr_func_t *lr_fn) {
    LLVMValueRef ll_inst;
    LLVMBasicBlockRef ll_bb;
    bc_func_ctx_t fnc;
    uint32_t block_seq = 0;
    unsigned nparams;
    unsigned i;

    memset(&fnc, 0, sizeof(fnc));
    fnc.bc = bc;
    fnc.func = lr_fn;

    for (ll_bb = LLVMGetFirstBasicBlock(ll_fn); ll_bb; ll_bb = LLVMGetNextBasicBlock(ll_bb)) {
        const char *bb_name = LLVMGetBasicBlockName(ll_bb);
        lr_block_t *lr_bb;
        char generated[32];
        if (!bb_name || bb_name[0] == '\0') {
            snprintf(generated, sizeof(generated), "bb%u", block_seq++);
            bb_name = generated;
        }
        lr_bb = lr_block_create(lr_fn, bc->arena, bb_name);
        if (!lr_bb || !block_map_put(&fnc.blocks, ll_bb, lr_bb)) {
            bc_set_error(bc, "failed to allocate basic block map");
            free(fnc.blocks.entries);
            free(fnc.values.entries);
            return false;
        }
    }

    nparams = LLVMCountParams(ll_fn);
    for (i = 0; i < nparams && i < lr_fn->num_params; i++) {
        LLVMValueRef ll_param = LLVMGetParam(ll_fn, i);
        if (!value_map_put(&fnc.values, ll_param, lr_fn->param_vregs[i], lr_fn->param_types[i])) {
            bc_set_error(bc, "failed to allocate parameter vreg map");
            free(fnc.blocks.entries);
            free(fnc.values.entries);
            return false;
        }
    }

    for (ll_bb = LLVMGetFirstBasicBlock(ll_fn); ll_bb; ll_bb = LLVMGetNextBasicBlock(ll_bb)) {
        for (ll_inst = LLVMGetFirstInstruction(ll_bb); ll_inst;
             ll_inst = LLVMGetNextInstruction(ll_inst)) {
            lr_type_t *ty = bc_convert_type(bc, LLVMTypeOf(ll_inst));
            uint32_t vreg;
            if (!ty) {
                free(fnc.blocks.entries);
                free(fnc.values.entries);
                return false;
            }
            if (ty->kind == LR_TYPE_VOID)
                continue;
            vreg = lr_vreg_new(lr_fn);
            if (!value_map_put(&fnc.values, ll_inst, vreg, ty)) {
                bc_set_error(bc, "failed to allocate instruction vreg map");
                free(fnc.blocks.entries);
                free(fnc.values.entries);
                return false;
            }
        }
    }

    for (ll_bb = LLVMGetFirstBasicBlock(ll_fn); ll_bb; ll_bb = LLVMGetNextBasicBlock(ll_bb)) {
        lr_block_t *lr_bb = block_map_get(&fnc.blocks, ll_bb);
        if (!lr_bb) {
            bc_set_error(bc, "internal error: missing block mapping");
            free(fnc.blocks.entries);
            free(fnc.values.entries);
            return false;
        }
        for (ll_inst = LLVMGetFirstInstruction(ll_bb); ll_inst;
             ll_inst = LLVMGetNextInstruction(ll_inst)) {
            if (!bc_translate_instruction(bc, &fnc, lr_bb, ll_inst)) {
                free(fnc.blocks.entries);
                free(fnc.values.entries);
                return false;
            }
        }
    }

    free(fnc.blocks.entries);
    free(fnc.values.entries);
    return true;
}

bool lr_bc_parser_available(void) {
    return true;
}

lr_module_t *lr_parse_bc_data(const uint8_t *data, size_t len,
                              lr_arena_t *arena, char *err, size_t errlen) {
    LLVMContextRef llctx = NULL;
    LLVMMemoryBufferRef membuf = NULL;
    LLVMModuleRef llmod = NULL;
    LLVMValueRef ll_fn;
    bc_ctx_t bc;

    if (!data && len != 0) {
        lr_frontend_set_error(err, errlen, "invalid bitcode input buffer");
        return NULL;
    }
    if (!arena) {
        lr_frontend_set_error(err, errlen, "arena is required for bitcode parse");
        return NULL;
    }
    if (!lr_bc_is_bitcode(data, len)) {
        lr_frontend_set_error(err, errlen, "input is not LLVM bitcode");
        return NULL;
    }

    memset(&bc, 0, sizeof(bc));
    bc.arena = arena;
    bc.err = err;
    bc.errlen = errlen;
    if (err && errlen > 0)
        err[0] = '\0';

    bc.module = lr_module_create(arena);
    if (!bc.module) {
        lr_frontend_set_error(err, errlen, "failed to allocate liric module");
        return NULL;
    }

    llctx = LLVMContextCreate();
    if (!llctx) {
        lr_frontend_set_error(err, errlen, "failed to create LLVM context");
        free(bc.entries);
        return NULL;
    }

    membuf = LLVMCreateMemoryBufferWithMemoryRangeCopy((const char *)data, len, "liric_bc_input");
    if (!membuf) {
        lr_frontend_set_error(err, errlen, "failed to create LLVM memory buffer");
        LLVMContextDispose(llctx);
        free(bc.entries);
        return NULL;
    }

    if (LLVMParseBitcodeInContext2(llctx, membuf, &llmod) != 0 || !llmod) {
        lr_frontend_set_error(err, errlen, "failed to parse LLVM bitcode");
        LLVMDisposeMemoryBuffer(membuf);
        LLVMContextDispose(llctx);
        free(bc.entries);
        return NULL;
    }

    /* Create function signatures first, then decode bodies. */
    for (ll_fn = LLVMGetFirstFunction(llmod); ll_fn; ll_fn = LLVMGetNextFunction(ll_fn)) {
        const char *name = NULL;
        size_t name_len = 0;
        char *owned_name;
        LLVMTypeRef fn_ty;
        lr_type_t *ret_ty;
        lr_type_t **params = NULL;
        unsigned nparams = 0;
        unsigned i;
        bool is_decl;
        lr_func_t *lr_fn;

        if (!bc_value_name(ll_fn, &name, &name_len)) {
            bc_set_error(&bc, "found function with empty name in bitcode");
            LLVMDisposeModule(llmod);
            LLVMDisposeMemoryBuffer(membuf);
            LLVMContextDispose(llctx);
            free(bc.entries);
            return NULL;
        }
        owned_name = lr_arena_strdup(arena, name, name_len);
        fn_ty = LLVMGlobalGetValueType(ll_fn);
        ret_ty = bc_convert_type(&bc, LLVMGetReturnType(fn_ty));
        if (!ret_ty) {
            LLVMDisposeModule(llmod);
            LLVMDisposeMemoryBuffer(membuf);
            LLVMContextDispose(llctx);
            free(bc.entries);
            return NULL;
        }

        nparams = LLVMCountParamTypes(fn_ty);
        if (nparams > 0) {
            LLVMTypeRef *ll_params = (LLVMTypeRef *)malloc((size_t)nparams * sizeof(*ll_params));
            params = lr_arena_array(arena, lr_type_t *, nparams);
            if (!ll_params || !params) {
                free(ll_params);
                bc_set_error(&bc, "out of memory while creating function signature");
                LLVMDisposeModule(llmod);
                LLVMDisposeMemoryBuffer(membuf);
                LLVMContextDispose(llctx);
                free(bc.entries);
                return NULL;
            }
            LLVMGetParamTypes(fn_ty, ll_params);
            for (i = 0; i < nparams; i++) {
                params[i] = bc_convert_type(&bc, ll_params[i]);
                if (!params[i]) {
                    free(ll_params);
                    LLVMDisposeModule(llmod);
                    LLVMDisposeMemoryBuffer(membuf);
                    LLVMContextDispose(llctx);
                    free(bc.entries);
                    return NULL;
                }
            }
            free(ll_params);
        }

        is_decl = LLVMIsDeclaration(ll_fn) != 0 || LLVMCountBasicBlocks(ll_fn) == 0;
        lr_fn = lr_frontend_create_function(bc.module, owned_name, ret_ty, params, nparams,
                                            LLVMIsFunctionVarArg(fn_ty) != 0, is_decl, NULL);
        if (!lr_fn) {
            bc_set_error(&bc, "failed to create liric function '%s'", owned_name);
            LLVMDisposeModule(llmod);
            LLVMDisposeMemoryBuffer(membuf);
            LLVMContextDispose(llctx);
            free(bc.entries);
            return NULL;
        }
    }

    {
        lr_func_t *cur_lr = bc.module->first_func;
        for (ll_fn = LLVMGetFirstFunction(llmod); ll_fn && cur_lr;
             ll_fn = LLVMGetNextFunction(ll_fn), cur_lr = cur_lr->next) {
            bool is_decl = LLVMIsDeclaration(ll_fn) != 0 || LLVMCountBasicBlocks(ll_fn) == 0;
            if (is_decl)
                continue;
            if (!bc_translate_function_body(&bc, ll_fn, cur_lr)) {
                LLVMDisposeModule(llmod);
                LLVMDisposeMemoryBuffer(membuf);
                LLVMContextDispose(llctx);
                free(bc.entries);
                return NULL;
            }
        }
    }

    LLVMDisposeModule(llmod);
    LLVMDisposeMemoryBuffer(membuf);
    LLVMContextDispose(llctx);
    free(bc.entries);
    return bc.module;
}

#endif
