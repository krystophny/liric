#include "target.h"
#include "ir.h"
#include "arena.h"

#if defined(__x86_64__) || defined(_M_X64)

#include "cp_template.h"
#include <string.h>
#include <stdbool.h>

/*
 * x86_64 copy-and-patch backend (Mode A).
 *
 * For each supported instruction, memcpy the pre-assembled template
 * into the code buffer and patch sentinel values with actual stack
 * offsets.  Falls back to ISel (compile_func) for any function that
 * contains an unsupported opcode.
 */

/* ---- extern labels from cp_templates_x86_64.S ---- */

#define DECL_TEMPLATE(name) \
    extern const uint8_t lr_cp_##name##_begin[]; \
    extern const uint8_t lr_cp_##name##_end[]

DECL_TEMPLATE(prologue);
DECL_TEMPLATE(ret_i64);
DECL_TEMPLATE(ret_i32);
DECL_TEMPLATE(ret_void);
DECL_TEMPLATE(add_i64);
DECL_TEMPLATE(add_i32);
DECL_TEMPLATE(sub_i64);
DECL_TEMPLATE(sub_i32);
DECL_TEMPLATE(and_i64);
DECL_TEMPLATE(and_i32);
DECL_TEMPLATE(or_i64);
DECL_TEMPLATE(or_i32);
DECL_TEMPLATE(xor_i64);
DECL_TEMPLATE(xor_i32);
DECL_TEMPLATE(mul_i64);
DECL_TEMPLATE(mul_i32);
DECL_TEMPLATE(sdiv_i64);
DECL_TEMPLATE(sdiv_i32);
DECL_TEMPLATE(srem_i64);
DECL_TEMPLATE(srem_i32);
DECL_TEMPLATE(shl_i64);
DECL_TEMPLATE(shl_i32);
DECL_TEMPLATE(lshr_i64);
DECL_TEMPLATE(lshr_i32);
DECL_TEMPLATE(ashr_i64);
DECL_TEMPLATE(ashr_i32);
DECL_TEMPLATE(store_param_rdi);
DECL_TEMPLATE(store_param_rsi);
DECL_TEMPLATE(store_param_rdx);
DECL_TEMPLATE(store_param_rcx);
DECL_TEMPLATE(store_param_r8);
DECL_TEMPLATE(store_param_r9);

#undef DECL_TEMPLATE

/* ISel fallback (defined in target_x86_64.c, exposed via target vtable) */
extern const lr_target_t *lr_target_x86_64(void);

/* ---- template table (initialized once) ---- */

typedef enum {
    CP_PROLOGUE,
    CP_RET_I64, CP_RET_I32, CP_RET_VOID,
    CP_ADD_I64, CP_ADD_I32,
    CP_SUB_I64, CP_SUB_I32,
    CP_AND_I64, CP_AND_I32,
    CP_OR_I64,  CP_OR_I32,
    CP_XOR_I64, CP_XOR_I32,
    CP_MUL_I64, CP_MUL_I32,
    CP_SDIV_I64, CP_SDIV_I32,
    CP_SREM_I64, CP_SREM_I32,
    CP_SHL_I64,  CP_SHL_I32,
    CP_LSHR_I64, CP_LSHR_I32,
    CP_ASHR_I64, CP_ASHR_I32,
    CP_STORE_PARAM_RDI,
    CP_STORE_PARAM_RSI,
    CP_STORE_PARAM_RDX,
    CP_STORE_PARAM_RCX,
    CP_STORE_PARAM_R8,
    CP_STORE_PARAM_R9,
    CP_NUM_TEMPLATES,
} cp_template_id_t;

static lr_cp_template_t g_templates[CP_NUM_TEMPLATES];
static bool g_templates_ready;

static void ensure_templates(void) {
    if (g_templates_ready) return;

#define INIT_T(id, name) \
    lr_cp_template_init(&g_templates[id], lr_cp_##name##_begin, lr_cp_##name##_end)

    INIT_T(CP_PROLOGUE,  prologue);
    INIT_T(CP_RET_I64,   ret_i64);
    INIT_T(CP_RET_I32,   ret_i32);
    INIT_T(CP_RET_VOID,  ret_void);
    INIT_T(CP_ADD_I64,   add_i64);
    INIT_T(CP_ADD_I32,   add_i32);
    INIT_T(CP_SUB_I64,   sub_i64);
    INIT_T(CP_SUB_I32,   sub_i32);
    INIT_T(CP_AND_I64,   and_i64);
    INIT_T(CP_AND_I32,   and_i32);
    INIT_T(CP_OR_I64,    or_i64);
    INIT_T(CP_OR_I32,    or_i32);
    INIT_T(CP_XOR_I64,   xor_i64);
    INIT_T(CP_XOR_I32,   xor_i32);
    INIT_T(CP_MUL_I64,   mul_i64);
    INIT_T(CP_MUL_I32,   mul_i32);
    INIT_T(CP_SDIV_I64,  sdiv_i64);
    INIT_T(CP_SDIV_I32,  sdiv_i32);
    INIT_T(CP_SREM_I64,  srem_i64);
    INIT_T(CP_SREM_I32,  srem_i32);
    INIT_T(CP_SHL_I64,   shl_i64);
    INIT_T(CP_SHL_I32,   shl_i32);
    INIT_T(CP_LSHR_I64,  lshr_i64);
    INIT_T(CP_LSHR_I32,  lshr_i32);
    INIT_T(CP_ASHR_I64,  ashr_i64);
    INIT_T(CP_ASHR_I32,  ashr_i32);
    INIT_T(CP_STORE_PARAM_RDI, store_param_rdi);
    INIT_T(CP_STORE_PARAM_RSI, store_param_rsi);
    INIT_T(CP_STORE_PARAM_RDX, store_param_rdx);
    INIT_T(CP_STORE_PARAM_RCX, store_param_rcx);
    INIT_T(CP_STORE_PARAM_R8,  store_param_r8);
    INIT_T(CP_STORE_PARAM_R9,  store_param_r9);

#undef INIT_T

    g_templates_ready = true;
}

/* ---- local compile context ---- */

typedef struct {
    uint8_t *buf;
    size_t buflen;
    size_t pos;
    uint32_t stack_size;
    int32_t *stack_slots;
    uint32_t num_stack_slots;
    lr_arena_t *arena;
} cp_ctx_t;

static size_t align_up_cp(size_t value, size_t align) {
    if (align <= 1) return value;
    return ((value + align - 1) / align) * align;
}

static int32_t cp_alloc_slot(cp_ctx_t *ctx, uint32_t vreg) {
    while (vreg >= ctx->num_stack_slots) {
        uint32_t old = ctx->num_stack_slots;
        uint32_t new_cap = old == 0 ? 64 : old * 2;
        int32_t *ns = lr_arena_array_uninit(ctx->arena, int32_t, new_cap);
        if (old > 0) memcpy(ns, ctx->stack_slots, old * sizeof(int32_t));
        for (uint32_t i = old; i < new_cap; i++) ns[i] = 0;
        ctx->stack_slots = ns;
        ctx->num_stack_slots = new_cap;
    }
    if (ctx->stack_slots[vreg] != 0)
        return ctx->stack_slots[vreg];

    ctx->stack_size = (uint32_t)align_up_cp(ctx->stack_size, 8);
    ctx->stack_size += 8;
    int32_t offset = -(int32_t)ctx->stack_size;
    ctx->stack_slots[vreg] = offset;
    return offset;
}

/* Emit a template into the code buffer, patching sentinel values. */
static void cp_emit(cp_ctx_t *ctx, const lr_cp_template_t *t,
                    int32_t src0_off, int32_t src1_off,
                    int32_t dest_off, int32_t imm32) {
    if (ctx->pos + t->code_len > ctx->buflen) {
        ctx->pos += t->code_len;
        return;
    }
    memcpy(ctx->buf + ctx->pos, t->code, t->code_len);

    for (uint8_t i = 0; i < t->num_patches; i++) {
        const lr_cp_patch_point_t *pp = &t->patches[i];
        int32_t val;
        switch (pp->operand_idx) {
        case 0: val = src0_off; break;
        case 1: val = src1_off; break;
        case 2: val = dest_off; break;
        case 3: val = imm32;    break;
        default: val = 0;       break;
        }
        uint8_t *patch_addr = ctx->buf + ctx->pos + pp->offset;
        memcpy(patch_addr, &val, 4);
    }
    ctx->pos += t->code_len;
}

/* Resolve an operand to a stack offset, materializing immediates if needed. */
static int32_t cp_resolve_operand(cp_ctx_t *ctx, const lr_operand_t *op) {
    if (op->kind == LR_VAL_VREG)
        return cp_alloc_slot(ctx, op->vreg);
    /* Immediate: store to a temp slot via raw encoding. */
    ctx->stack_size = (uint32_t)align_up_cp(ctx->stack_size, 8);
    ctx->stack_size += 8;
    int32_t off = -(int32_t)ctx->stack_size;
    if (ctx->pos + 11 <= ctx->buflen) {
        /* mov qword [rbp + disp32], imm32 (sign-extended to 64-bit) */
        /* REX.W(48) C7 ModRM(85) disp32 imm32 = 11 bytes */
        uint8_t *p = ctx->buf + ctx->pos;
        p[0] = 0x48; p[1] = 0xC7; p[2] = 0x85;
        memcpy(p + 3, &off, 4);
        int32_t imm = (int32_t)op->imm_i64;
        memcpy(p + 7, &imm, 4);
    }
    ctx->pos += 11;
    return off;
}

/* Map (opcode, is_i32) -> template id, or -1 if unsupported. */
static int cp_template_for_alu(lr_opcode_t op, bool is_i32) {
    switch (op) {
    case LR_OP_ADD:  return is_i32 ? CP_ADD_I32  : CP_ADD_I64;
    case LR_OP_SUB:  return is_i32 ? CP_SUB_I32  : CP_SUB_I64;
    case LR_OP_AND:  return is_i32 ? CP_AND_I32  : CP_AND_I64;
    case LR_OP_OR:   return is_i32 ? CP_OR_I32   : CP_OR_I64;
    case LR_OP_XOR:  return is_i32 ? CP_XOR_I32  : CP_XOR_I64;
    case LR_OP_MUL:  return is_i32 ? CP_MUL_I32  : CP_MUL_I64;
    case LR_OP_SDIV: return is_i32 ? CP_SDIV_I32 : CP_SDIV_I64;
    case LR_OP_SREM: return is_i32 ? CP_SREM_I32 : CP_SREM_I64;
    case LR_OP_SHL:  return is_i32 ? CP_SHL_I32  : CP_SHL_I64;
    case LR_OP_LSHR: return is_i32 ? CP_LSHR_I32 : CP_LSHR_I64;
    case LR_OP_ASHR: return is_i32 ? CP_ASHR_I32 : CP_ASHR_I64;
    default: return -1;
    }
}

/* Check if all instructions in a function have C&P templates. */
static bool cp_function_supported(lr_func_t *func) {
    if (!func || func->num_blocks == 0) return false;
    if (func->num_blocks > 1) return false;
    if (func->num_params > 6) return false;
    if (func->vararg) return false;

    lr_block_t *b = func->block_array[0];
    for (uint32_t i = 0; i < b->num_insts; i++) {
        lr_inst_t *inst = b->inst_array[i];
        switch (inst->op) {
        case LR_OP_RET:
        case LR_OP_RET_VOID:
            break;
        case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
        case LR_OP_OR:  case LR_OP_XOR: case LR_OP_MUL:
        case LR_OP_SDIV: case LR_OP_SREM:
        case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
            if (!inst->type) return false;
            lr_type_kind_t k = inst->type->kind;
            if (k != LR_TYPE_I32 && k != LR_TYPE_I64) return false;
            if (inst->num_operands < 2) return false;
            for (uint32_t oi = 0; oi < 2; oi++) {
                lr_operand_kind_t vk = inst->operands[oi].kind;
                if (vk != LR_VAL_VREG && vk != LR_VAL_IMM_I64) return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

static const cp_template_id_t param_store_templates[6] = {
    CP_STORE_PARAM_RDI, CP_STORE_PARAM_RSI, CP_STORE_PARAM_RDX,
    CP_STORE_PARAM_RCX, CP_STORE_PARAM_R8,  CP_STORE_PARAM_R9,
};

int x86_64_compile_func_cp(lr_func_t *func, lr_module_t *mod,
                           uint8_t *buf, size_t buflen, size_t *out_len,
                           lr_arena_t *arena) {
    ensure_templates();

    /* Fall back to ISel for unsupported functions. */
    if (!cp_function_supported(func))
        return lr_target_x86_64()->compile_func(func, mod, buf, buflen, out_len, arena);

    uint32_t initial = func->next_vreg > 64 ? func->next_vreg : 64;
    cp_ctx_t ctx = {
        .buf = buf,
        .buflen = buflen,
        .pos = 0,
        .stack_size = 0,
        .stack_slots = lr_arena_array(arena, int32_t, initial),
        .num_stack_slots = initial,
        .arena = arena,
    };

    /* Reserve prologue space — we patch frame size at the end. */
    const lr_cp_template_t *pro = &g_templates[CP_PROLOGUE];
    size_t prologue_pos = ctx.pos;
    cp_emit(&ctx, pro, 0, 0, 0, 0);

    /* Store incoming parameters to stack slots. */
    for (uint32_t i = 0; i < func->num_params && i < 6; i++) {
        int32_t dest_off = cp_alloc_slot(&ctx, func->param_vregs[i]);
        cp_emit(&ctx, &g_templates[param_store_templates[i]],
                0, 0, dest_off, 0);
    }

    /* Emit instructions. */
    lr_block_t *b = func->block_array[0];
    for (uint32_t ii = 0; ii < b->num_insts; ii++) {
        lr_inst_t *inst = b->inst_array[ii];
        switch (inst->op) {
        case LR_OP_RET: {
            const lr_operand_t *rv = &inst->operands[0];
            int32_t src_off = cp_resolve_operand(&ctx, rv);
            bool is_i32 = inst->type && inst->type->kind == LR_TYPE_I32;
            cp_emit(&ctx, &g_templates[is_i32 ? CP_RET_I32 : CP_RET_I64],
                    src_off, 0, 0, 0);
            break;
        }
        case LR_OP_RET_VOID:
            cp_emit(&ctx, &g_templates[CP_RET_VOID], 0, 0, 0, 0);
            break;
        case LR_OP_ADD: case LR_OP_SUB: case LR_OP_AND:
        case LR_OP_OR:  case LR_OP_XOR: case LR_OP_MUL:
        case LR_OP_SDIV: case LR_OP_SREM:
        case LR_OP_SHL: case LR_OP_LSHR: case LR_OP_ASHR: {
            bool is_i32 = inst->type && inst->type->kind == LR_TYPE_I32;
            int tid = cp_template_for_alu(inst->op, is_i32);
            int32_t src0 = cp_resolve_operand(&ctx, &inst->operands[0]);
            int32_t src1 = cp_resolve_operand(&ctx, &inst->operands[1]);
            int32_t dest = cp_alloc_slot(&ctx, inst->dest);
            cp_emit(&ctx, &g_templates[tid], src0, src1, dest, 0);
            break;
        }
        default:
            return -1;
        }
    }

    /* Patch prologue frame size (16-byte aligned). */
    uint32_t frame = (uint32_t)align_up_cp(ctx.stack_size, 16);
    if (frame < 16) frame = 16;
    int32_t frame_i32 = (int32_t)frame;
    for (uint8_t i = 0; i < pro->num_patches; i++) {
        if (pro->patches[i].operand_idx == 3) {
            memcpy(buf + prologue_pos + pro->patches[i].offset, &frame_i32, 4);
            break;
        }
    }

    if (ctx.pos > buflen)
        return -1;
    *out_len = ctx.pos;
    return 0;
}

#else /* !x86_64 */

int x86_64_compile_func_cp(lr_func_t *func, lr_module_t *mod,
                           uint8_t *buf, size_t buflen, size_t *out_len,
                           lr_arena_t *arena) {
    (void)func; (void)mod; (void)buf; (void)buflen; (void)out_len; (void)arena;
    return -1;
}

#endif
