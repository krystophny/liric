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

    /* global/function symbol name -> id mapping */
    struct { char *name; uint32_t id; } global_map[4096];
    uint32_t global_map_count;

    /* function name -> func mapping */
    struct { char *name; lr_func_t *func; } func_map[1024];
    uint32_t func_map_count;

    /* named type alias mapping (e.g. %string_descriptor -> struct type) */
    struct { char *name; lr_type_t *type; } type_map[256];
    uint32_t type_map_count;

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

static void register_global(lr_parser_t *p, const char *name, uint32_t id) {
    if (p->global_map_count < 4096) {
        p->global_map[p->global_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->global_map[p->global_map_count].id = id;
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

static void register_type(lr_parser_t *p, const char *name, lr_type_t *ty) {
    if (p->type_map_count < 256) {
        p->type_map[p->type_map_count].name = lr_arena_strdup(p->arena, name, strlen(name));
        p->type_map[p->type_map_count].type = ty;
        p->type_map_count++;
    }
}

static lr_type_t *resolve_type(lr_parser_t *p, const char *name) {
    for (uint32_t i = 0; i < p->type_map_count; i++) {
        if (strcmp(p->type_map[i].name, name) == 0)
            return p->type_map[i].type;
    }
    return NULL;
}

static lr_type_t *parse_type(lr_parser_t *p);
static lr_operand_t parse_typed_operand(lr_parser_t *p);
static void skip_balanced_parens(lr_parser_t *p);
static void skip_balanced_braces(lr_parser_t *p);
static void skip_balanced_brackets(lr_parser_t *p);

static lr_type_t *parse_type(lr_parser_t *p) {
    lr_token_t t = p->cur;
    lr_type_t *ty = NULL;
    switch (t.kind) {
    case LR_TOK_VOID:   next(p); ty = p->module->type_void; break;
    case LR_TOK_I1:     next(p); ty = p->module->type_i1; break;
    case LR_TOK_I8:     next(p); ty = p->module->type_i8; break;
    case LR_TOK_I16:    next(p); ty = p->module->type_i16; break;
    case LR_TOK_I32:    next(p); ty = p->module->type_i32; break;
    case LR_TOK_I64:    next(p); ty = p->module->type_i64; break;
    case LR_TOK_FLOAT:  next(p); ty = p->module->type_float; break;
    case LR_TOK_DOUBLE: next(p); ty = p->module->type_double; break;
    case LR_TOK_PTR:    next(p); ty = p->module->type_ptr; break;
    case LR_TOK_LOCAL_ID: {
        char *tname = tok_name(p, &p->cur);
        next(p);
        lr_type_t *resolved = resolve_type(p, tname);
        ty = resolved ? resolved : p->module->type_ptr;
        break;
    }
    case LR_TOK_LBRACKET: {
        next(p);
        int64_t count = p->cur.int_val;
        expect(p, LR_TOK_INT_LIT);
        expect(p, LR_TOK_X);
        lr_type_t *elem = parse_type(p);
        expect(p, LR_TOK_RBRACKET);
        ty = lr_type_array(p->arena, elem, count);
        break;
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
        ty = lr_type_struct(p->arena, fields, nf, false, NULL);
        break;
    }
    case LR_TOK_LANGLE: {
        next(p);
        if (check(p, LR_TOK_INT_LIT)) {
            /* Vector type: <N x T> */
            int64_t count = p->cur.int_val;
            expect(p, LR_TOK_INT_LIT);
            expect(p, LR_TOK_X);
            lr_type_t *elem = parse_type(p);
            expect(p, LR_TOK_RANGLE);
            ty = lr_type_array(p->arena, elem, count);
        } else {
            /* Packed struct: <{ ... }> */
            expect(p, LR_TOK_LBRACE);
            lr_type_t *fields[256];
            uint32_t nf = 0;
            if (!check(p, LR_TOK_RBRACE)) {
                fields[nf++] = parse_type(p);
                while (match(p, LR_TOK_COMMA))
                    fields[nf++] = parse_type(p);
            }
            expect(p, LR_TOK_RBRACE);
            expect(p, LR_TOK_RANGLE);
            ty = lr_type_struct(p->arena, fields, nf, true, NULL);
        }
        break;
    }
    default:
        error(p, "expected type, got '%s'", lr_tok_name(t.kind));
        ty = p->module->type_void;
        break;
    }

    /* Handle type suffixes: pointers and function types.
     * Examples: i8*, i8**, i32 (i64)*, i8* (i32)* */
    while (true) {
        if (match(p, LR_TOK_STAR)) {
            /* Typed pointer suffix or pointer to function */
            ty = p->module->type_ptr;
        } else if (check(p, LR_TOK_LPAREN)) {
            /* Function type: RetType (ParamTypes...)
             * Note: ty is the return type at this point */
            next(p);
            lr_type_t *ret = ty;
            lr_type_t *params[256];
            uint32_t nparams = 0;
            bool vararg = false;

            if (!check(p, LR_TOK_RPAREN)) {
                if (check(p, LR_TOK_DOTDOTDOT)) {
                    vararg = true;
                    next(p);
                } else {
                    params[nparams++] = parse_type(p);
                    while (match(p, LR_TOK_COMMA)) {
                        if (check(p, LR_TOK_DOTDOTDOT)) {
                            vararg = true;
                            next(p);
                            break;
                        }
                        params[nparams++] = parse_type(p);
                    }
                }
            }
            expect(p, LR_TOK_RPAREN);
            ty = lr_type_func(p->arena, ret, params, nparams, vararg);
        } else {
            break;
        }
    }

    return ty;
}

static bool is_bare_identifier(const lr_token_t *tok) {
    if (tok->kind != LR_TOK_LOCAL_ID || tok->len == 0)
        return false;
    return tok->start[0] != '%' && tok->start[0] != '@';
}

static void skip_attr_payload(lr_parser_t *p) {
    if (!check(p, LR_TOK_LPAREN))
        return;
    skip_balanced_parens(p);
}

/* Skip attribute annotations we don't care about */
static void skip_attrs(lr_parser_t *p) {
    while (true) {
        if (p->cur.kind == LR_TOK_NSW || p->cur.kind == LR_TOK_NUW ||
            p->cur.kind == LR_TOK_INBOUNDS || p->cur.kind == LR_TOK_NONNULL ||
            p->cur.kind == LR_TOK_NOUNDEF || p->cur.kind == LR_TOK_SIGNEXT ||
            p->cur.kind == LR_TOK_ZEROEXT || p->cur.kind == LR_TOK_NOCAPTURE ||
            p->cur.kind == LR_TOK_READONLY || p->cur.kind == LR_TOK_WRITEONLY ||
            p->cur.kind == LR_TOK_NNAN || p->cur.kind == LR_TOK_NINF ||
            p->cur.kind == LR_TOK_NSZ || p->cur.kind == LR_TOK_DSOLOCAL ||
            p->cur.kind == LR_TOK_ATTR_GROUP || p->cur.kind == LR_TOK_METADATA_ID) {
            next(p);
            continue;
        }
        if (p->cur.kind == LR_TOK_ALIGN) {
            next(p);
            if (check(p, LR_TOK_INT_LIT))
                next(p);
            continue;
        }
        if (is_bare_identifier(&p->cur)) {
            next(p);
            skip_attr_payload(p);
            continue;
        }
        break;
    }
}

static lr_operand_t parse_const_gep_operand(lr_parser_t *p, lr_type_t *result_ty) {
    bool wrapped = false;
    expect(p, LR_TOK_GETELEMENTPTR);
    skip_attrs(p);
    if (match(p, LR_TOK_LPAREN))
        wrapped = true;
    (void)parse_type(p);
    expect(p, LR_TOK_COMMA);

    lr_operand_t base = parse_typed_operand(p);
    while (match(p, LR_TOK_COMMA))
        (void)parse_typed_operand(p);
    if (wrapped)
        expect(p, LR_TOK_RPAREN);

    if (base.kind == LR_VAL_GLOBAL)
        return lr_op_global(base.global_id, result_ty);
    if (base.kind == LR_VAL_VREG)
        return lr_op_vreg(base.vreg, result_ty);
    if (base.kind == LR_VAL_NULL)
        return lr_op_null(result_ty);
    return lr_op_null(result_ty);
}

static lr_operand_t parse_aggregate_constant_operand(lr_parser_t *p, lr_type_t *type) {
    if (check(p, LR_TOK_LBRACE))
        skip_balanced_braces(p);
    else
        skip_balanced_brackets(p);
    return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
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
    if (check(p, LR_TOK_STRING_LIT)) {
        next(p);
        return lr_op_null(type);
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
        uint32_t gid = resolve_global(p, name);
        if (gid == UINT32_MAX) {
            gid = lr_module_intern_symbol(p->module, name);
            register_global(p, name, gid);
        }
        return lr_op_global(gid, type);
    }
    if (check(p, LR_TOK_GETELEMENTPTR))
        return parse_const_gep_operand(p, type);
    if (check(p, LR_TOK_BITCAST) || check(p, LR_TOK_INTTOPTR) ||
        check(p, LR_TOK_PTRTOINT) || check(p, LR_TOK_SEXT) ||
        check(p, LR_TOK_ZEXT) || check(p, LR_TOK_TRUNC) ||
        check(p, LR_TOK_SITOFP) || check(p, LR_TOK_FPTOSI) ||
        check(p, LR_TOK_FPEXT) || check(p, LR_TOK_FPTRUNC)) {
        next(p);
        expect(p, LR_TOK_LPAREN);
        lr_operand_t src = parse_typed_operand(p);
        expect(p, LR_TOK_TO);
        (void)parse_type(p);
        expect(p, LR_TOK_RPAREN);
        src.type = type;
        return src;
    }
    if (check(p, LR_TOK_LBRACE) || check(p, LR_TOK_LBRACKET))
        return parse_aggregate_constant_operand(p, type);
    if (check(p, LR_TOK_LANGLE)) {
        /* packed struct literal: <{ ... }> */
        next(p);  /* consume < */
        if (!check(p, LR_TOK_LBRACE)) {
            error(p, "expected '{' after '<' in packed struct literal");
            return lr_op_imm_i64(0, type);
        }
        skip_balanced_braces(p);
        expect(p, LR_TOK_RANGLE);
        return (lr_operand_t){ .kind = LR_VAL_UNDEF, .type = type };
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

static void skip_balanced_parens(lr_parser_t *p) {
    uint32_t depth = 0;
    expect(p, LR_TOK_LPAREN);
    depth = 1;
    while (depth > 0 && !check(p, LR_TOK_EOF)) {
        if (match(p, LR_TOK_LPAREN)) {
            depth++;
            continue;
        }
        if (match(p, LR_TOK_RPAREN)) {
            depth--;
            continue;
        }
        next(p);
    }
    if (depth != 0)
        error(p, "unterminated parenthesized type in call");
}

static void skip_balanced_braces(lr_parser_t *p) {
    uint32_t depth = 0;
    expect(p, LR_TOK_LBRACE);
    depth = 1;
    while (depth > 0 && !check(p, LR_TOK_EOF)) {
        if (match(p, LR_TOK_LBRACE)) {
            depth++;
            continue;
        }
        if (match(p, LR_TOK_RBRACE)) {
            depth--;
            continue;
        }
        next(p);
    }
    if (depth != 0)
        error(p, "unterminated aggregate constant");
}

static void skip_balanced_brackets(lr_parser_t *p) {
    uint32_t depth = 0;
    expect(p, LR_TOK_LBRACKET);
    depth = 1;
    while (depth > 0 && !check(p, LR_TOK_EOF)) {
        if (match(p, LR_TOK_LBRACKET)) {
            depth++;
            continue;
        }
        if (match(p, LR_TOK_RBRACKET)) {
            depth--;
            continue;
        }
        next(p);
    }
    if (depth != 0)
        error(p, "unterminated array constant");
}

static void skip_optional_callee_signature(lr_parser_t *p) {
    /*
     * Accept typed callee signatures like:
     *   call ptr (ptr, i64, ...) @foo(...)
     *   call i32 (i32)* @fn(i32 1)
     */
    if (check(p, LR_TOK_LPAREN)) {
        skip_balanced_parens(p);
        while (match(p, LR_TOK_STAR)) {}
        skip_attrs(p);
    }
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
            case LR_TOK_SDIV: case LR_TOK_SREM: case LR_TOK_UREM:
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
                case LR_TOK_UREM: irop = LR_OP_SREM; break;
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
                lr_operand_t count_op = {0};
                bool has_count = false;
                /* check for optional count: ", <inttype> <operand>" */
                if (match(p, LR_TOK_COMMA)) {
                    if (check(p, LR_TOK_ALIGN)) {
                        /* just align, no count */
                        next(p); next(p);
                    } else {
                        /* parse count operand */
                        lr_type_t *count_ty = parse_type(p);
                        count_op = parse_operand(p, count_ty);
                        has_count = true;
                        /* check for optional ", align N" after count */
                        if (match(p, LR_TOK_COMMA)) {
                            if (check(p, LR_TOK_ALIGN)) { next(p); next(p); }
                        }
                    }
                }
                lr_inst_t *inst;
                if (has_count) {
                    lr_operand_t ops[1] = {count_op};
                    inst = lr_inst_create(p->arena, LR_OP_ALLOCA,
                        p->module->type_ptr, dest, ops, 1);
                } else {
                    inst = lr_inst_create(p->arena, LR_OP_ALLOCA,
                        p->module->type_ptr, dest, NULL, 0);
                }
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
                skip_optional_callee_signature(p);
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

            case LR_TOK_FNEG: {
                lr_operand_t src = parse_typed_operand(p);
                lr_operand_t ops[1] = {src};
                lr_inst_t *inst = lr_inst_create(p->arena, LR_OP_FNEG,
                    src.type, dest, ops, 1);
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
                case LR_TOK_FALSE: pred = LR_FCMP_FALSE; break;
                case LR_TOK_OEQ: pred = LR_FCMP_OEQ; break;
                case LR_TOK_OGT: pred = LR_FCMP_OGT; break;
                case LR_TOK_OGE: pred = LR_FCMP_OGE; break;
                case LR_TOK_OLT: pred = LR_FCMP_OLT; break;
                case LR_TOK_OLE: pred = LR_FCMP_OLE; break;
                case LR_TOK_ONE: pred = LR_FCMP_ONE; break;
                case LR_TOK_ORD: pred = LR_FCMP_ORD; break;
                case LR_TOK_UEQ: pred = LR_FCMP_UEQ; break;
                case LR_TOK_UGT: pred = LR_FCMP_UGT; break;
                case LR_TOK_UGE: pred = LR_FCMP_UGE; break;
                case LR_TOK_ULT: pred = LR_FCMP_ULT; break;
                case LR_TOK_ULE: pred = LR_FCMP_ULE; break;
                case LR_TOK_UNE: pred = LR_FCMP_UNE; break;
                case LR_TOK_UNO: pred = LR_FCMP_UNO; break;
                case LR_TOK_TRUE: pred = LR_FCMP_TRUE; break;
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
        skip_optional_callee_signature(p);
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

    /* register parameter vregs: named params get only name, unnamed get numeric alias */
    for (uint32_t i = 0; i < func->num_params; i++) {
        if (param_names && param_names[i]) {
            /* named parameter: register only the name, not numeric alias */
            if (p->vreg_map_count < 4096) {
                p->vreg_map[p->vreg_map_count].name = param_names[i];
                p->vreg_map[p->vreg_map_count].id = func->param_vregs[i];
                p->vreg_map_count++;
            }
        } else {
            /* unnamed parameter: register numeric alias */
            char buf[32];
            snprintf(buf, sizeof(buf), "%u", i);
            if (p->vreg_map_count < 4096) {
                p->vreg_map[p->vreg_map_count].name = lr_arena_strdup(p->arena, buf, strlen(buf));
                p->vreg_map[p->vreg_map_count].id = func->param_vregs[i];
                p->vreg_map_count++;
            }
        }
    }

    expect(p, LR_TOK_LBRACE);

    /* first block - may be unlabeled */
    lr_block_t *cur_block = NULL;

    while (!check(p, LR_TOK_RBRACE) && !check(p, LR_TOK_EOF) && !p->had_error) {
        /* Check for label: name followed by colon */
        if (check(p, LR_TOK_LOCAL_ID) || check(p, LR_TOK_STRING_LIT)) {
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
    if (resolve_global(p, name) == UINT32_MAX) {
        uint32_t sym_id = lr_module_intern_symbol(p->module, name);
        register_global(p, name, sym_id);
    }
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
        bool at_toplevel_col = (p->cur.col == 1);
        if (at_toplevel_col && (check(p, LR_TOK_DEFINE) || check(p, LR_TOK_DECLARE)))
            return;
        if (at_toplevel_col && (check(p, LR_TOK_GLOBAL_ID) || check(p, LR_TOK_LOCAL_ID)))
            return;
        next(p);
    }
}

static size_t struct_field_offset(const lr_type_t *st, uint32_t field_idx) {
    size_t off = 0;
    for (uint32_t i = 0; i < field_idx && i < st->struc.num_fields; i++) {
        size_t fsz = lr_type_size(st->struc.fields[i]);
        if (!st->struc.packed) {
            size_t fa = lr_type_align(st->struc.fields[i]);
            if (fa > 0)
                off = (off + fa - 1) & ~(fa - 1);
        }
        off += fsz;
    }
    if (field_idx < st->struc.num_fields && !st->struc.packed) {
        size_t fa = lr_type_align(st->struc.fields[field_idx]);
        if (fa > 0)
            off = (off + fa - 1) & ~(fa - 1);
    }
    return off;
}

static void parse_aggregate_initializer(lr_parser_t *p, lr_global_t *g,
                                         uint8_t *buf, size_t buf_size,
                                         const lr_type_t *ty, size_t base_offset);

/*
 * Parse a single scalar initializer value at field_off within buf.
 * Handles: integers, floats, GEP, bare global refs, null, undef, nested aggregates.
 * Records relocations for pointer-to-global values.
 */
static void parse_init_field_value(lr_parser_t *p, lr_global_t *g,
                                    uint8_t *buf, size_t buf_size,
                                    const lr_type_t *field_type, size_t field_off) {
    size_t field_sz = lr_type_size(field_type);

    if (check(p, LR_TOK_LANGLE) || check(p, LR_TOK_LBRACE) ||
        check(p, LR_TOK_LBRACKET)) {
        parse_aggregate_initializer(p, g, buf, buf_size, field_type, field_off);
    } else if (check(p, LR_TOK_GETELEMENTPTR)) {
        lr_operand_t gep = parse_const_gep_operand(p, p->module->type_ptr);
        if (gep.kind == LR_VAL_GLOBAL) {
            const char *ref = lr_module_symbol_name(p->module, gep.global_id);
            if (ref) {
                lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
                r->offset = field_off;
                r->symbol_name = lr_arena_strdup(p->arena, ref, strlen(ref));
                r->next = g->relocs;
                g->relocs = r;
            }
        }
    } else if (check(p, LR_TOK_INT_LIT)) {
        int64_t val = p->cur.int_val;
        next(p);
        if (field_off + field_sz <= buf_size)
            memcpy(buf + field_off, &val, field_sz < 8 ? field_sz : 8);
    } else if (check(p, LR_TOK_FLOAT_LIT)) {
        double val = p->cur.float_val;
        next(p);
        if (field_type->kind == LR_TYPE_FLOAT) {
            float fv = (float)val;
            if (field_off + 4 <= buf_size)
                memcpy(buf + field_off, &fv, 4);
        } else {
            if (field_off + 8 <= buf_size)
                memcpy(buf + field_off, &val, 8);
        }
    } else if (check(p, LR_TOK_NULL)) {
        next(p);
    } else if (check(p, LR_TOK_ZEROINITIALIZER)) {
        next(p);
    } else if (check(p, LR_TOK_GLOBAL_ID)) {
        char *ref_name = tok_name(p, &p->cur);
        next(p);
        uint32_t gid = resolve_global(p, ref_name);
        if (gid == UINT32_MAX) {
            gid = lr_module_intern_symbol(p->module, ref_name);
            register_global(p, ref_name, gid);
        }
        lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
        r->offset = field_off;
        r->symbol_name = lr_arena_strdup(p->arena, ref_name, strlen(ref_name));
        r->next = g->relocs;
        g->relocs = r;
    } else if (check(p, LR_TOK_UNDEF) || check(p, LR_TOK_STRING_LIT)) {
        next(p);
    } else {
        next(p);
    }
}

/*
 * Parse an aggregate constant initializer and write field values into buf.
 * Record relocations for pointer-to-global fields on the global g.
 * base_offset is the byte offset of this aggregate within the top-level global.
 */
static void parse_aggregate_initializer(lr_parser_t *p, lr_global_t *g,
                                         uint8_t *buf, size_t buf_size,
                                         const lr_type_t *ty, size_t base_offset) {
    bool packed_struct = false;

    if (check(p, LR_TOK_LANGLE)) {
        next(p);
        expect(p, LR_TOK_LBRACE);
        packed_struct = true;
    } else if (check(p, LR_TOK_LBRACE)) {
        next(p);
    } else if (check(p, LR_TOK_LBRACKET)) {
        next(p);
        if (ty->kind == LR_TYPE_ARRAY) {
            size_t elem_sz = lr_type_size(ty->array.elem);
            for (uint64_t i = 0; i < ty->array.count; i++) {
                if (check(p, LR_TOK_RBRACKET))
                    break;
                (void)parse_type(p);
                skip_attrs(p);
                size_t elem_off = base_offset + i * elem_sz;
                parse_init_field_value(p, g, buf, buf_size, ty->array.elem, elem_off);
                if (!match(p, LR_TOK_COMMA))
                    break;
            }
        }
        expect(p, LR_TOK_RBRACKET);
        return;
    } else {
        return;
    }

    if (ty->kind != LR_TYPE_STRUCT) {
        uint32_t depth = 1;
        while (depth > 0 && !check(p, LR_TOK_EOF)) {
            if (match(p, LR_TOK_LBRACE)) { depth++; continue; }
            if (match(p, LR_TOK_RBRACE)) { depth--; continue; }
            next(p);
        }
        if (packed_struct)
            match(p, LR_TOK_RANGLE);
        return;
    }

    for (uint32_t fi = 0; fi < ty->struc.num_fields; fi++) {
        if (check(p, LR_TOK_RBRACE))
            break;
        (void)parse_type(p);
        skip_attrs(p);
        size_t field_off = base_offset + struct_field_offset(ty, fi);
        parse_init_field_value(p, g, buf, buf_size, ty->struc.fields[fi], field_off);
        if (!match(p, LR_TOK_COMMA))
            break;
    }

    expect(p, LR_TOK_RBRACE);
    if (packed_struct)
        expect(p, LR_TOK_RANGLE);
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
    } else if (check(p, LR_TOK_TYPE)) {
        next(p);
        if (check(p, LR_TOK_OPAQUE)) {
            next(p);
        } else {
            lr_type_t *alias = parse_type(p);
            register_type(p, name, alias);
        }
        skip_line(p);
        return;
    } else {
        skip_line(p);
        return;
    }

    lr_type_t *ty = parse_type(p);
    lr_global_t *g = lr_global_create(p->module, name, ty, is_const);
    uint32_t sym_id = lr_module_intern_symbol(p->module, g->name);
    if (resolve_global(p, g->name) == UINT32_MAX)
        register_global(p, g->name, sym_id);

    if (check(p, LR_TOK_STRING_LIT)) {
        const char *s = p->cur.start;
        size_t slen = p->cur.len;
        if (slen >= 3 && s[0] == 'c' && s[1] == '"') {
            s += 2; slen -= 3;
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, slen + 1);
            size_t out = 0;
            for (size_t i = 0; i < slen; i++) {
                if (s[i] == '\\' && i + 2 < slen) {
                    int hi = 0, lo = 0;
                    char c1 = s[i + 1], c2 = s[i + 2];
                    hi = (c1 >= '0' && c1 <= '9') ? c1 - '0' :
                         (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 :
                         (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
                    lo = (c2 >= '0' && c2 <= '9') ? c2 - '0' :
                         (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 :
                         (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
                    if (hi >= 0 && lo >= 0) {
                        buf[out++] = (uint8_t)(hi * 16 + lo);
                        i += 2;
                        continue;
                    }
                }
                buf[out++] = (uint8_t)s[i];
            }
            g->init_data = buf;
            g->init_size = out;
        }
        next(p);
    } else if (check(p, LR_TOK_ZEROINITIALIZER)) {
        next(p);
    } else if (check(p, LR_TOK_INT_LIT)) {
        int64_t val = p->cur.int_val;
        size_t sz = lr_type_size(ty);
        if (sz > 0 && sz <= 8) {
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
            memcpy(buf, &val, sz);
            g->init_data = buf;
            g->init_size = sz;
        }
        next(p);
    } else if (check(p, LR_TOK_FLOAT_LIT)) {
        double val = p->cur.float_val;
        size_t sz = lr_type_size(ty);
        if (sz > 0) {
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
            memset(buf, 0, sz);
            if (ty->kind == LR_TYPE_FLOAT) {
                float fv = (float)val;
                memcpy(buf, &fv, 4);
            } else {
                memcpy(buf, &val, sz < 8 ? sz : 8);
            }
            g->init_data = buf;
            g->init_size = sz;
        }
        next(p);
    } else if (check(p, LR_TOK_LANGLE) || check(p, LR_TOK_LBRACE) ||
               check(p, LR_TOK_LBRACKET)) {
        size_t sz = lr_type_size(ty);
        if (sz > 0) {
            uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
            memset(buf, 0, sz);
            g->init_data = buf;
            g->init_size = sz;
            parse_aggregate_initializer(p, g, buf, sz, ty, 0);
        } else {
            if (check(p, LR_TOK_LBRACE))
                skip_balanced_braces(p);
            else if (check(p, LR_TOK_LBRACKET))
                skip_balanced_brackets(p);
            else {
                next(p);
                skip_balanced_braces(p);
                match(p, LR_TOK_RANGLE);
            }
        }
    } else if (check(p, LR_TOK_NULL)) {
        next(p);
    } else if (check(p, LR_TOK_GETELEMENTPTR)) {
        lr_operand_t gep = parse_const_gep_operand(p, p->module->type_ptr);
        size_t sz = lr_type_size(ty);
        if (sz == 0)
            sz = 8;
        uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
        memset(buf, 0, sz);
        g->init_data = buf;
        g->init_size = sz;
        if (gep.kind == LR_VAL_GLOBAL) {
            const char *ref = lr_module_symbol_name(p->module, gep.global_id);
            if (ref) {
                lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
                r->offset = 0;
                r->symbol_name = lr_arena_strdup(p->arena, ref, strlen(ref));
                r->next = g->relocs;
                g->relocs = r;
            }
        }
    } else if (check(p, LR_TOK_GLOBAL_ID)) {
        char *ref_name = tok_name(p, &p->cur);
        next(p);
        uint32_t gid = resolve_global(p, ref_name);
        if (gid == UINT32_MAX) {
            gid = lr_module_intern_symbol(p->module, ref_name);
            register_global(p, ref_name, gid);
        }
        size_t sz = lr_type_size(ty);
        if (sz == 0)
            sz = 8;
        uint8_t *buf = lr_arena_array(p->arena, uint8_t, sz);
        memset(buf, 0, sz);
        g->init_data = buf;
        g->init_size = sz;
        lr_reloc_t *r = lr_arena_new(p->arena, lr_reloc_t);
        r->offset = 0;
        r->symbol_name = lr_arena_strdup(p->arena, ref_name, strlen(ref_name));
        r->next = g->relocs;
        g->relocs = r;
    }

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
        } else if (check(&p, LR_TOK_LOCAL_ID)) {
            /* type alias: %name = type ... */
            char *tname = tok_name(&p, &p.cur);
            next(&p);
            if (match(&p, LR_TOK_EQUALS) && match(&p, LR_TOK_TYPE)) {
                if (check(&p, LR_TOK_OPAQUE)) {
                    next(&p);
                } else {
                    lr_type_t *alias = parse_type(&p);
                    register_type(&p, tname, alias);
                }
            }
            skip_line(&p);
        } else {
            /* skip unknown top-level directives (source_filename, target, attributes, metadata) */
            skip_line(&p);
        }
    }

    if (p.had_error) return NULL;
    return p.module;
}
