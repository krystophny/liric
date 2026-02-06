#include "ll_parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct lr_parser {
    lr_lexer_t lex;
    lr_token_t cur;
    lr_token_t prev;
    lr_arena_t *arena;
    lr_module_t *module;
    char *err;
    size_t errlen;
    bool had_error;

    /* vreg name -> id mapping for current function */
    struct { char *name; uint32_t id; } vreg_map[4096];
    uint32_t vreg_map_count;

    /* block name -> id mapping for current function */
    struct { char *name; uint32_t id; lr_block_t *block; } block_map[1024];
    uint32_t block_map_count;

    /* global name -> id mapping */
    struct { char *name; uint32_t id; lr_global_t *global; } global_map[4096];
    uint32_t global_map_count;

    /* function name -> func mapping */
    struct { char *name; lr_func_t *func; } func_map[1024];
    uint32_t func_map_count;

    lr_func_t *cur_func;
} lr_parser_t;

static void error(lr_parser_t *p, const char *fmt, ...) {
    if (p->had_error) return;
    p->had_error = true;
    if (p->err && p->errlen > 0) {
        va_list ap;
        va_start(ap, fmt);
        int n = snprintf(p->err, p->errlen, "line %u col %u: ",
                         p->cur.line, p->cur.col);
        if (n > 0 && (size_t)n < p->errlen)
            vsnprintf(p->err + n, p->errlen - n, fmt, ap);
        va_end(ap);
    }
}

static void next(lr_parser_t *p) {
    p->prev = p->cur;
    p->cur = lr_lexer_next(&p->lex);
}

static bool check(lr_parser_t *p, lr_tok_t kind) {
    return p->cur.kind == kind;
}

static bool match(lr_parser_t *p, lr_tok_t kind) {
    if (p->cur.kind == kind) { next(p); return true; }
    return false;
}

static void expect(lr_parser_t *p, lr_tok_t kind) {
    if (!match(p, kind))
        error(p, "expected '%s', got '%s'", lr_tok_name(kind), lr_tok_name(p->cur.kind));
}

/* Extract name from %name or @name token (skip the prefix character) */
static char *tok_name(lr_parser_t *p, const lr_token_t *t) {
    const char *s = t->start;
    size_t len = t->len;
    /* skip % or @ prefix */
    if (len > 0 && (s[0] == '%' || s[0] == '@')) {
        s++;
        len--;
    }
    /* skip quotes if present */
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
        s++;
        len -= 2;
    }
    return lr_arena_strdup(p->arena, s, len);
}

static uint32_t resolve_vreg(lr_parser_t *p, const char *name) {
    for (uint32_t i = 0; i < p->vreg_map_count; i++) {
        if (strcmp(p->vreg_map[i].name, name) == 0)
            return p->vreg_map[i].id;
    }
    /* auto-create */
    uint32_t id = lr_vreg_new(p->cur_func);
    if (p->vreg_map_count < 4096) {
        p->vreg_map[p->vreg_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->vreg_map[p->vreg_map_count].id = id;
        p->vreg_map_count++;
    }
    return id;
}

static uint32_t resolve_block(lr_parser_t *p, const char *name) {
    for (uint32_t i = 0; i < p->block_map_count; i++) {
        if (strcmp(p->block_map[i].name, name) == 0)
            return p->block_map[i].id;
    }
    /* forward reference: create block */
    lr_block_t *b = lr_block_create(p->cur_func, p->arena, name);
    if (p->block_map_count < 1024) {
        p->block_map[p->block_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->block_map[p->block_map_count].id = b->id;
        p->block_map[p->block_map_count].block = b;
        p->block_map_count++;
    }
    return b->id;
}

static lr_block_t *resolve_block_ptr(lr_parser_t *p, const char *name) {
    for (uint32_t i = 0; i < p->block_map_count; i++) {
        if (strcmp(p->block_map[i].name, name) == 0)
            return p->block_map[i].block;
    }
    lr_block_t *b = lr_block_create(p->cur_func, p->arena, name);
    if (p->block_map_count < 1024) {
        p->block_map[p->block_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->block_map[p->block_map_count].id = b->id;
        p->block_map[p->block_map_count].block = b;
        p->block_map_count++;
    }
    return b;
}

static uint32_t resolve_global(lr_parser_t *p, const char *name) {
    for (uint32_t i = 0; i < p->global_map_count; i++) {
        if (strcmp(p->global_map[i].name, name) == 0)
            return p->global_map[i].id;
    }
    return UINT32_MAX;
}

static void register_global(lr_parser_t *p, const char *name, lr_global_t *g) {
    if (p->global_map_count < 4096) {
        p->global_map[p->global_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->global_map[p->global_map_count].id = g->id;
        p->global_map[p->global_map_count].global = g;
        p->global_map_count++;
    }
}

static void register_func(lr_parser_t *p, const char *name, lr_func_t *f) {
    if (p->func_map_count < 1024) {
        p->func_map[p->func_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->func_map[p->func_map_count].func = f;
        p->func_map_count++;
    }
}

static lr_type_t *parse_type(lr_parser_t *p);

static lr_type_t *parse_type(lr_parser_t *p) {
    lr_token_t t = p->cur;
    switch (t.kind) {
    case LR_TOK_VOID:   next(p); return p->module->type_void;
    case LR_TOK_I1:     next(p); return p->module->type_i1;
    case LR_TOK_I8:     next(p); return p->module->type_i8;
    case LR_TOK_I16:    next(p); return p->module->type_i16;
    case LR_TOK_I32:    next(p); return p->module->type_i32;
    case LR_TOK_I64:    next(p); return p->module->type_i64;
    case LR_TOK_FLOAT:  next(p); return p->module->type_float;
    case LR_TOK_DOUBLE: next(p); return p->module->type_double;
    case LR_TOK_PTR:    next(p); return p->module->type_ptr;
    case LR_TOK_LBRACKET: {
        next(p);
        int64_t count = p->cur.int_val;
        expect(p, LR_TOK_INT_LIT);
        expect(p, LR_TOK_X);
        lr_type_t *elem = parse_type(p);
        expect(p, LR_TOK_RBRACKET);
        return lr_type_array(p->arena, elem, count);
    }
    case LR_TOK_LBRACE: {
        next(p);
        lr_type_t *fields[256];
        uint32_t nf = 0;
        if (!check(p, LR_TOK_RBRACE)) {
            fields[nf++] = parse_type(p);
            while (match(p, LR_TOK_COMMA))
                fields[nf++] = parse_type(p);
        }
        expect(p, LR_TOK_RBRACE);
        return lr_type_struct(p->arena, fields, nf, false, NULL);
    }
    default:
        error(p, "expected type, got '%s'", lr_tok_name(t.kind));
        return p->module->type_void;
    }
}

/* Skip attribute annotations we don't care about */
static void skip_attrs(lr_parser_t *p) {
    while (p->cur.kind == LR_TOK_NSW || p->cur.kind == LR_TOK_NUW ||
           p->cur.kind == LR_TOK_INBOUNDS || p->cur.kind == LR_TOK_NONNULL ||
           p->cur.kind == LR_TOK_NOUNDEF || p->cur.kind == LR_TOK_SIGNEXT ||
           p->cur.kind == LR_TOK_ZEROEXT || p->cur.kind == LR_TOK_NOCAPTURE ||
           p->cur.kind == LR_TOK_READONLY || p->cur.kind == LR_TOK_WRITEONLY ||
           p->cur.kind == LR_TOK_NNAN || p->cur.kind == LR_TOK_NINF ||
           p->cur.kind == LR_TOK_NSZ || p->cur.kind == LR_TOK_DSOLOCAL ||
           p->cur.kind == LR_TOK_ATTR_GROUP)
        next(p);
}

static lr_operand_t parse_operand(lr_parser_t *p, lr_type_t *type) {
    if (check(p, LR_TOK_INT_LIT)) {
        int64_t val = p->cur.int_val;
        next(p);
        return lr_op_imm_i64(val, type);
    }
    if (check(p, LR_TOK_FLOAT_LIT)) {
        double val = p->cur.float_val;
        next(p);
        return lr_op_imm_f64(val, type);
    }
    if (check(p, LR_TOK_TRUE)) {
        next(p);
        return lr_op_imm_i64(1, type);
    }
    if (check(p, LR_TOK_FALSE)) {
        next(p);
        return lr_op_imm_i64(0, type);
    }
    if (check(p, LR_TOK_NULL)) {
        next(p);
        return lr_op_null(type);
    }
    if (check(p, LR_TOK_UNDEF)) {
        next(p);
        return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
    }
    if (check(p, LR_TOK_ZEROINITIALIZER)) {
        next(p);
        return lr_op_imm_i64(0, type);
    }
    if (check(p, LR_TOK_LOCAL_ID)) {
        char *name = tok_name(p, &p->cur);
        next(p);
        uint32_t vreg = resolve_vreg(p, name);
        return lr_op_vreg(vreg, type);
    }
    if (check(p, LR_TOK_GLOBAL_ID)) {
        char *name = tok_name(p, &p->cur);
        next(p);
        /* check if it is a function reference */
        uint32_t gid = resolve_global(p, name);
        if (gid != UINT32_MAX)
            return lr_op_global(gid, type);
        /* might be a function - return as global with max id for now */
        return lr_op_global(UINT32_MAX, type);
    }
    error(p, "expected operand, got '%s'", lr_tok_name(p->cur.kind));
    return lr_op_imm_i64(0, type);
}

/* Parse a typed operand: type value */
static lr_operand_t parse_typed_operand(lr_parser_t *p) {
    lr_type_t *t = parse_type(p);
    skip_attrs(p);
    return parse_operand(p, t);
}

static void parse_instruction(lr_parser_t *p, lr_block_t *block) {
    /* Check for label: */
    if (check(p, LR_TOK_LOCAL_ID)) {
        /* Could be: %x = ... or a label. Peek ahead for = */
        lr_token_t saved = p->cur;
        next(p);
        if (check(p, LR_TOK_EQUALS)) {
            /* %x = instruction */
            next(p);
            char *dest_name = tok_name(p, &saved);
            uint32_t dest = resolve_vreg(p, dest_name);

            lr_tok_t op_tok = p->cur.kind;
            next(p);
            skip_attrs(p);

            switch (op_tok) {
            case LR_TOK_ADD: case LR_TOK_SUB: case LR_TOK_MUL:
            case LR_TOK_SDIV: case LR_TOK_SREM:
            case LR_TOK_AND: case LR_TOK_OR: case LR_TOK_XOR:
            case LR_TOK_SHL: case LR_TOK_LSHR: case LR_TOK_ASHR:
            case LR_TOK_FADD: case LR_TOK_FSUB:
            case LR_TOK_FMUL: case LR_TOK_FDIV: {
                skip_attrs(p);
                lr_type_t *ty = parse_type(p);
                skip_attrs(p);
                lr_operand_t lhs = parse_operand(p, ty);
                expect(p, LR_TOK_COMMA);
                lr_operand_t rhs = parse_operand(p, ty);

                lr_opcode_t irop;
                switch (op_tok) {
                case LR_TOK_ADD:  irop = LR_OP_ADD; break;
                case LR_TOK_SUB:  irop = LR_OP_SUB; break;
                case LR_TOK_MUL:  irop = LR_OP_MUL; break;
                case LR_TOK_SDIV: irop = LR_OP_SDIV; break;
                case LR_TOK_SREM: irop = LR_OP_SREM; break;
                case LR_TOK_AND:  irop = LR_OP_AND; break;
                case LR_TOK_OR:   irop = LR_OP_OR; break;
                case LR_TOK_XOR:  irop = LR_OP_XOR; break;
                case LR_TOK_SHL:  irop = LR_OP_SHL; break;
                case LR_TOK_LSHR: irop = LR_OP_LSHR; break;
                case LR_TOK_ASHR: irop = LR_OP_ASHR; break;
                case LR_TOK_FADD: irop = LR_OP_FADD; break;
                case LR_TOK_FSUB: irop = LR_OP_FSUB; break;
                case LR_TOK_FMUL: irop = LR_OP_FMUL; break;
                case LR_TOK_FDIV: irop = LR_OP_FDIV; break;
                default: irop = LR_OP_ADD; break;
                }

                lr_operand_t ops[2] = {lhs, rhs};
                lr_inst_t *inst = lr_inst_create(p->arena, irop, ty, dest, ops, 2);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_ICMP: {
                lr_icmp_pred_t pred;
                switch (p->cur.kind) {
                case LR_TOK_EQ:  pred = LR_ICMP_EQ; break;
                case LR_TOK_NE:  pred = LR_ICMP_NE; break;
                case LR_TOK_SGT: pred = LR_ICMP_SGT; break;
                case LR_TOK_SGE: pred = LR_ICMP_SGE; break;
                case LR_TOK_SLT: pred = LR_ICMP_SLT; break;
                case LR_TOK_SLE: pred = LR_ICMP_SLE; break;
                case LR_TOK_UGT: pred = LR_ICMP_UGT; break;
                case LR_TOK_UGE: pred = LR_ICMP_UGE; break;
                case LR_TOK_ULT: pred = LR_ICMP_ULT; break;
                case LR_TOK_ULE: pred = LR_ICMP_ULE; break;
                default:
                    error(p, "expected icmp predicate");
                    pred = LR_ICMP_EQ;
                }
                next(p);
                lr_type_t *ty = parse_type(p);
                lr_operand_t lhs = parse_operand(p, ty);
                expect(p, LR_TOK_COMMA);
                lr_operand_t rhs = parse_operand(p, ty);
                lr_operand_t ops[2] = {lhs, rhs};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_ICMP,
                    p->module->type_i1, dest, ops, 2);
                inst->icmp_pred = pred;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_ALLOCA: {
                lr_type_t *ty = parse_type(p);
                /* skip optional ", align N" */
                if (match(p, LR_TOK_COMMA)) {
                    if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
                }
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_ALLOCA,
                    p->module->type_ptr, dest, NULL, 0);
                inst->type = ty;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_LOAD: {
                lr_type_t *ty = parse_type(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t src = parse_typed_operand(p);
                /* skip optional ", align N" */
                if (match(p, LR_TOK_COMMA)) {
                    if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
                }
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_LOAD,
                    ty, dest, ops, 1);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_CALL: {
                lr_type_t *ret_ty = parse_type(p);
                skip_attrs(p);
                /* optional function type */
                /* callee */
                lr_operand_t callee = parse_operand(p, p->module->type_ptr);
                expect(p, LR_TOK_LPAREN);
                lr_operand_t args[64];
                uint32_t nargs = 0;
                if (!check(p, LR_TOK_RPAREN)) {
                    args[nargs++] = parse_typed_operand(p);
                    while (match(p, LR_TOK_COMMA)) {
                        skip_attrs(p);
                        args[nargs++] = parse_typed_operand(p);
                    }
                }
                expect(p, LR_TOK_RPAREN);
                /* args[0..nargs-1] are the actual args, callee is separate */
                lr_operand_t all_ops[65];
                all_ops[0] = callee;
                for (uint32_t i = 0; i < nargs; i++)
                    all_ops[i + 1] = args[i];
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CALL,
                    ret_ty, dest, all_ops, nargs + 1);
                lr_block_append(block, inst);
                /* skip trailing attribute groups */
                skip_attrs(p);
                break;
            }

            case LR_TOK_SEXT: case LR_TOK_ZEXT: case LR_TOK_TRUNC:
            case LR_TOK_BITCAST: case LR_TOK_PTRTOINT: case LR_TOK_INTTOPTR:
            case LR_TOK_SITOFP: case LR_TOK_FPTOSI:
            case LR_TOK_FPEXT: case LR_TOK_FPTRUNC: {
                lr_operand_t src = parse_typed_operand(p);
                expect(p, LR_TOK_TO);
                lr_type_t *dst_ty = parse_type(p);
                lr_opcode_t irop;
                switch (op_tok) {
                case LR_TOK_SEXT:     irop = LR_OP_SEXT; break;
                case LR_TOK_ZEXT:     irop = LR_OP_ZEXT; break;
                case LR_TOK_TRUNC:    irop = LR_OP_TRUNC; break;
                case LR_TOK_BITCAST:  irop = LR_OP_BITCAST; break;
                case LR_TOK_PTRTOINT: irop = LR_OP_PTRTOINT; break;
                case LR_TOK_INTTOPTR: irop = LR_OP_INTTOPTR; break;
                case LR_TOK_SITOFP:   irop = LR_OP_SITOFP; break;
                case LR_TOK_FPTOSI:   irop = LR_OP_FPTOSI; break;
                case LR_TOK_FPEXT:    irop = LR_OP_FPEXT; break;
                case LR_TOK_FPTRUNC:  irop = LR_OP_FPTRUNC; break;
                default: irop = LR_OP_BITCAST; break;
                }
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, irop,
                    dst_ty, dest, ops, 1);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_SELECT: {
                lr_operand_t cond = parse_typed_operand(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t tv = parse_typed_operand(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t fv = parse_typed_operand(p);
                lr_operand_t ops[3] = {cond, tv, fv};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_SELECT,
                    tv.type, dest, ops, 3);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_GETELEMENTPTR: {
                skip_attrs(p);
                lr_type_t *base_ty = parse_type(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t ops[16];
                uint32_t nops = 0;
                ops[nops++] = parse_typed_operand(p);
                while (match(p, LR_TOK_COMMA))
                    ops[nops++] = parse_typed_operand(p);
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_GEP,
                    p->module->type_ptr, dest, ops, nops);
                /* store base type in inst->type for GEP offset computation */
                inst->type = base_ty;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_PHI: {
                lr_type_t *ty = parse_type(p);
                lr_operand_t ops[64];
                uint32_t nops = 0;
                /* [ val, %label ] pairs */
                do {
                    expect(p, LR_TOK_LBRACKET);
                    ops[nops++] = parse_operand(p, ty);
                    expect(p, LR_TOK_COMMA);
                    /* block label */
                    if (check(p, LR_TOK_LOCAL_ID)) {
                        char *bname = tok_name(p, &p->cur);
                        next(p);
                        uint32_t bid = resolve_block(p, bname);
                        ops[nops++] = lr_op_block(bid);
                    }
                    expect(p, LR_TOK_RBRACKET);
                } while (match(p, LR_TOK_COMMA));
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_PHI,
                    ty, dest, ops, nops);
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_EXTRACTVALUE: {
                lr_operand_t src = parse_typed_operand(p);
                uint32_t indices[16];
                uint32_t nidx = 0;
                while (match(p, LR_TOK_COMMA)) {
                    indices[nidx++] = (uint32_t)p->cur.int_val;
                    expect(p, LR_TOK_INT_LIT);
                }
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_EXTRACTVALUE,
                    p->module->type_i64, dest, ops, 1);
                inst->indices = lr_arena_array(p->arena, uint32_t, nidx);
                memcpy(inst->indices, indices, sizeof(uint32_t) * nidx);
                inst->num_indices = nidx;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_INSERTVALUE: {
                lr_operand_t agg = parse_typed_operand(p);
                expect(p, LR_TOK_COMMA);
                lr_operand_t val = parse_typed_operand(p);
                uint32_t indices[16];
                uint32_t nidx = 0;
                while (match(p, LR_TOK_COMMA)) {
                    indices[nidx++] = (uint32_t)p->cur.int_val;
                    expect(p, LR_TOK_INT_LIT);
                }
                lr_operand_t ops[2] = {agg, val};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_INSERTVALUE,
                    agg.type, dest, ops, 2);
                inst->indices = lr_arena_array(p->arena, uint32_t, nidx);
                memcpy(inst->indices, indices, sizeof(uint32_t) * nidx);
                inst->num_indices = nidx;
                lr_block_append(block, inst);
                break;
            }

            case LR_TOK_FCMP: {
                lr_fcmp_pred_t pred;
                switch (p->cur.kind) {
                case LR_TOK_OEQ: pred = LR_FCMP_OEQ; break;
                case LR_TOK_ONE: pred = LR_FCMP_ONE; break;
                case LR_TOK_OGT: pred = LR_FCMP_OGT; break;
                case LR_TOK_OGE: pred = LR_FCMP_OGE; break;
                case LR_TOK_OLT: pred = LR_FCMP_OLT; break;
                case LR_TOK_OLE: pred = LR_FCMP_OLE; break;
                case LR_TOK_UNO: pred = LR_FCMP_UNO; break;
                default:
                    error(p, "expected fcmp predicate");
                    pred = LR_FCMP_OEQ;
                }
                next(p);
                lr_type_t *ty = parse_type(p);
                lr_operand_t lhs = parse_operand(p, ty);
                expect(p, LR_TOK_COMMA);
                lr_operand_t rhs = parse_operand(p, ty);
                lr_operand_t ops[2] = {lhs, rhs};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_FCMP,
                    p->module->type_i1, dest, ops, 2);
                inst->fcmp_pred = pred;
                lr_block_append(block, inst);
                break;
            }

            default:
                error(p, "unknown instruction '%.*s'", (int)p->prev.len, p->prev.start);
                break;
            }
            return;
        }
        /* Not an assignment, rewind. This is a label reference being used
           as a bare identifier - shouldn't happen at instruction level.
           Let's re-process as a label. */
        p->cur = saved;
        /* fall through to label check below */
    }

    /* terminators and void instructions */
    lr_tok_t op_tok = p->cur.kind;

    if (op_tok == LR_TOK_RET) {
        next(p);
        if (check(p, LR_TOK_VOID)) {
            next(p);
            lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_RET_VOID,
                p->module->type_void, 0, NULL, 0);
            lr_block_append(block, inst);
        } else {
            lr_operand_t val = parse_typed_operand(p);
            lr_operand_t ops[1] = {val};
            lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_RET,
                val.type, 0, ops, 1);
            lr_block_append(block, inst);
        }
        return;
    }

    if (op_tok == LR_TOK_BR) {
        next(p);
        if (check(p, LR_TOK_I1)) {
            /* conditional: br i1 %cond, label %t, label %f */
            next(p);
            lr_operand_t cond = parse_operand(p, p->module->type_i1);
            expect(p, LR_TOK_COMMA);
            expect(p, LR_TOK_LABEL);
            if (check(p, LR_TOK_LOCAL_ID)) {
                char *tname = tok_name(p, &p->cur);
                next(p);
                uint32_t tid = resolve_block(p, tname);
                expect(p, LR_TOK_COMMA);
                expect(p, LR_TOK_LABEL);
                char *fname = tok_name(p, &p->cur);
                next(p);
                uint32_t fid = resolve_block(p, fname);
                lr_operand_t ops[3] = {cond, lr_op_block(tid), lr_op_block(fid)};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CONDBR,
                    p->module->type_void, 0, ops, 3);
                lr_block_append(block, inst);
            }
        } else {
            /* unconditional: br label %dest */
            expect(p, LR_TOK_LABEL);
            if (check(p, LR_TOK_LOCAL_ID)) {
                char *dname = tok_name(p, &p->cur);
                next(p);
                uint32_t did = resolve_block(p, dname);
                lr_operand_t ops[1] = {lr_op_block(did)};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_BR,
                    p->module->type_void, 0, ops, 1);
                lr_block_append(block, inst);
            }
        }
        return;
    }

    if (op_tok == LR_TOK_STORE) {
        next(p);
        lr_operand_t val = parse_typed_operand(p);
        expect(p, LR_TOK_COMMA);
        lr_operand_t dst = parse_typed_operand(p);
        /* skip optional ", align N" */
        if (match(p, LR_TOK_COMMA)) {
            if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
        }
        lr_operand_t ops[2] = {val, dst};
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_STORE,
            p->module->type_void, 0, ops, 2);
        lr_block_append(block, inst);
        return;
    }

    if (op_tok == LR_TOK_UNREACHABLE) {
        next(p);
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_UNREACHABLE,
            p->module->type_void, 0, NULL, 0);
        lr_block_append(block, inst);
        return;
    }

    /* void call */
    if (op_tok == LR_TOK_CALL) {
        next(p);
        lr_type_t *ret_ty = parse_type(p);
        skip_attrs(p);
        lr_operand_t callee = parse_operand(p, p->module->type_ptr);
        expect(p, LR_TOK_LPAREN);
        lr_operand_t args[64];
        uint32_t nargs = 0;
        if (!check(p, LR_TOK_RPAREN)) {
            args[nargs++] = parse_typed_operand(p);
            while (match(p, LR_TOK_COMMA)) {
                skip_attrs(p);
                args[nargs++] = parse_typed_operand(p);
            }
        }
        expect(p, LR_TOK_RPAREN);
        lr_operand_t all_ops[65];
        all_ops[0] = callee;
        for (uint32_t i = 0; i < nargs; i++)
            all_ops[i + 1] = args[i];
        lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_CALL,
            ret_ty, 0, all_ops, nargs + 1);
        lr_block_append(block, inst);
        skip_attrs(p);
        return;
    }

    error(p, "unexpected token '%s' in basic block", lr_tok_name(op_tok));
}

static void parse_function_body(lr_parser_t *p, lr_func_t *func, char **param_names) {
    p->cur_func = func;
    p->vreg_map_count = 0;
    p->block_map_count = 0;

    /* register parameter vregs with both numeric and named aliases */
    for (uint32_t i = 0; i < func->num_params; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u", i);
        if (p->vreg_map_count < 4096) {
            p->vreg_map[p->vreg_map_count].name = lr_arena_strdup(p->arena, buf, strlen(buf));
            p->vreg_map[p->vreg_map_count].id = func->param_vregs[i];
            p->vreg_map_count++;
        }
        if (param_names && param_names[i] && p->vreg_map_count < 4096) {
            p->vreg_map[p->vreg_map_count].name = param_names[i];
            p->vreg_map[p->vreg_map_count].id = func->param_vregs[i];
            p->vreg_map_count++;
        }
    }

    expect(p, LR_TOK_LBRACE);

    /* first block - may be unlabeled */
    lr_block_t *cur_block = NULL;

    while (!check(p, LR_TOK_RBRACE) && !check(p, LR_TOK_EOF) && !p->had_error) {
        /* Check for label: name followed by colon */
        if (check(p, LR_TOK_LOCAL_ID)) {
            /* peek: is next token a colon? */
            lr_token_t saved_tok = p->cur;
            size_t saved_pos = p->lex.pos;
            uint32_t saved_line = p->lex.line;
            uint32_t saved_col = p->lex.col;

            next(p);
            if (check(p, LR_TOK_COLON)) {
                /* it is a label */
                next(p);
                char *bname = tok_name(p, &saved_tok);
                cur_block = resolve_block_ptr(p, bname);
                continue;
            }
            /* not a label, restore and parse as instruction */
            p->cur = saved_tok;
            p->lex.pos = saved_pos;
            p->lex.line = saved_line;
            p->lex.col = saved_col;
        }

        /* Check for bare keyword label (e.g. "entry:" without %) */
        /* In LLVM IR, block labels can be bare identifiers */
        /* We handle this by checking known patterns */

        if (!cur_block) {
            cur_block = resolve_block_ptr(p, "entry");
        }

        parse_instruction(p, cur_block);
    }

    expect(p, LR_TOK_RBRACE);
    p->cur_func = NULL;
}

static lr_type_t *parse_param_type(lr_parser_t *p) {
    lr_type_t *ty = parse_type(p);
    skip_attrs(p);
    return ty;
}

static void parse_function_def(lr_parser_t *p, bool is_decl) {
    skip_attrs(p);
    lr_type_t *ret_type = parse_type(p);

    if (!check(p, LR_TOK_GLOBAL_ID)) {
        error(p, "expected function name");
        return;
    }
    char *name = tok_name(p, &p->cur);
    next(p);

    expect(p, LR_TOK_LPAREN);
    lr_type_t *params[256];
    char *param_names[256];
    uint32_t nparams = 0;
    bool vararg = false;
    memset(param_names, 0, sizeof(param_names));
    if (!check(p, LR_TOK_RPAREN)) {
        if (check(p, LR_TOK_DOTDOTDOT)) {
            vararg = true;
            next(p);
        } else {
            params[nparams] = parse_param_type(p);
            if (check(p, LR_TOK_LOCAL_ID)) {
                param_names[nparams] = tok_name(p, &p->cur);
                next(p);
            }
            nparams++;
            while (match(p, LR_TOK_COMMA)) {
                if (check(p, LR_TOK_DOTDOTDOT)) {
                    vararg = true;
                    next(p);
                    break;
                }
                skip_attrs(p);
                params[nparams] = parse_param_type(p);
                if (check(p, LR_TOK_LOCAL_ID)) {
                    param_names[nparams] = tok_name(p, &p->cur);
                    next(p);
                }
                nparams++;
            }
        }
    }
    expect(p, LR_TOK_RPAREN);

    /* skip trailing attrs like unnamed_addr #0 */
    skip_attrs(p);
    while (check(p, LR_TOK_UNNAMED_ADDR) || check(p, LR_TOK_LOCAL_UNNAMED_ADDR)) next(p);
    skip_attrs(p);

    lr_func_t *func;
    if (is_decl) {
        func = lr_func_declare(p->module, name, ret_type, params, nparams, vararg);
    } else {
        func = lr_func_create(p->module, name, ret_type, params, nparams, vararg);
    }
    register_func(p, name, func);

    if (!is_decl)
        parse_function_body(p, func, param_names);
}

/* Skip lines we don't understand (metadata, target triple, attributes, etc.) */
static void skip_line(lr_parser_t *p) {
    /* Skip tokens until we hit something that looks like a new top-level construct */
    while (!check(p, LR_TOK_EOF)) {
        if (check(p, LR_TOK_DEFINE) || check(p, LR_TOK_DECLARE))
            return;
        /* Skip globals starting with @ */
        if (check(p, LR_TOK_GLOBAL_ID)) {
            /* might be a global def, but we handle that in the main loop */
            return;
        }
        next(p);
    }
}

static void parse_global(lr_parser_t *p) {
    char *name = tok_name(p, &p->cur);
    next(p);
    expect(p, LR_TOK_EQUALS);

    /* skip linkage */
    while (check(p, LR_TOK_EXTERNAL) || check(p, LR_TOK_INTERNAL) ||
           check(p, LR_TOK_PRIVATE) || check(p, LR_TOK_COMMON) ||
           check(p, LR_TOK_LINKONCE_ODR) || check(p, LR_TOK_DSOLOCAL) ||
           check(p, LR_TOK_UNNAMED_ADDR) || check(p, LR_TOK_LOCAL_UNNAMED_ADDR))
        next(p);

    bool is_const = false;
    if (check(p, LR_TOK_GLOBAL)) {
        next(p);
    } else if (check(p, LR_TOK_CONSTANT)) {
        next(p);
        is_const = true;
    } else {
        /* might be a type alias: %name = type { ... } */
        skip_line(p);
        return;
    }

    lr_type_t *ty = parse_type(p);
    lr_global_t *g = lr_global_create(p->module, name, ty, is_const);
    register_global(p, name, g);

    /* skip initializer for now */
    skip_line(p);
}

lr_module_t *lr_parse_ll_text(const char *src, size_t len,
                               lr_arena_t *arena, char *err, size_t errlen) {
    lr_parser_t p = {0};
    lr_lexer_init(&p.lex, src, len);
    p.arena = arena;
    p.err = err;
    p.errlen = errlen;
    if (err && errlen > 0) err[0] = '\0';

    p.module = lr_module_create(arena);
    next(&p);

    while (!check(&p, LR_TOK_EOF) && !p.had_error) {
        if (check(&p, LR_TOK_DEFINE)) {
            next(&p);
            parse_function_def(&p, false);
        } else if (check(&p, LR_TOK_DECLARE)) {
            next(&p);
            parse_function_def(&p, true);
        } else if (check(&p, LR_TOK_GLOBAL_ID)) {
            parse_global(&p);
        } else {
            /* skip unknown top-level directives (source_filename, target, attributes, metadata) */
            skip_line(&p);
        }
    }

    if (p.had_error) return NULL;
    return p.module;
}
