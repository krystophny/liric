#include "ll_lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

void lr_lexer_init(lr_lexer_t *lex, const char *src, size_t len) {
    lex->src = src;
    lex->src_len = len;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
}

static char peek(lr_lexer_t *lex) {
    if (lex->pos >= lex->src_len) return '\0';
    return lex->src[lex->pos];
}

static char advance(lr_lexer_t *lex) {
    if (lex->pos >= lex->src_len) return '\0';
    char c = lex->src[lex->pos++];
    if (c == '\n') { lex->line++; lex->col = 1; }
    else lex->col++;
    return c;
}

static void skip_whitespace_and_comments(lr_lexer_t *lex) {
    while (lex->pos < lex->src_len) {
        char c = peek(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lex);
        } else if (c == ';') {
            while (lex->pos < lex->src_len && peek(lex) != '\n')
                advance(lex);
        } else {
            break;
        }
    }
}

typedef struct { const char *name; lr_tok_t tok; } keyword_t;

static const keyword_t keywords[] = {
    {"define", LR_TOK_DEFINE},
    {"declare", LR_TOK_DECLARE},
    {"ret", LR_TOK_RET},
    {"br", LR_TOK_BR},
    {"label", LR_TOK_LABEL},
    {"add", LR_TOK_ADD},
    {"sub", LR_TOK_SUB},
    {"mul", LR_TOK_MUL},
    {"sdiv", LR_TOK_SDIV},
    {"srem", LR_TOK_SREM},
    {"urem", LR_TOK_UREM},
    {"and", LR_TOK_AND},
    {"or", LR_TOK_OR},
    {"xor", LR_TOK_XOR},
    {"shl", LR_TOK_SHL},
    {"lshr", LR_TOK_LSHR},
    {"ashr", LR_TOK_ASHR},
    {"fadd", LR_TOK_FADD},
    {"fsub", LR_TOK_FSUB},
    {"fmul", LR_TOK_FMUL},
    {"fdiv", LR_TOK_FDIV},
    {"fneg", LR_TOK_FNEG},
    {"icmp", LR_TOK_ICMP},
    {"fcmp", LR_TOK_FCMP},
    {"alloca", LR_TOK_ALLOCA},
    {"load", LR_TOK_LOAD},
    {"store", LR_TOK_STORE},
    {"getelementptr", LR_TOK_GETELEMENTPTR},
    {"call", LR_TOK_CALL},
    {"phi", LR_TOK_PHI},
    {"select", LR_TOK_SELECT},
    {"sext", LR_TOK_SEXT},
    {"zext", LR_TOK_ZEXT},
    {"trunc", LR_TOK_TRUNC},
    {"bitcast", LR_TOK_BITCAST},
    {"ptrtoint", LR_TOK_PTRTOINT},
    {"inttoptr", LR_TOK_INTTOPTR},
    {"sitofp", LR_TOK_SITOFP},
    {"fptosi", LR_TOK_FPTOSI},
    {"fpext", LR_TOK_FPEXT},
    {"fptrunc", LR_TOK_FPTRUNC},
    {"extractvalue", LR_TOK_EXTRACTVALUE},
    {"insertvalue", LR_TOK_INSERTVALUE},
    {"unreachable", LR_TOK_UNREACHABLE},
    {"to", LR_TOK_TO},
    {"align", LR_TOK_ALIGN},
    {"nsw", LR_TOK_NSW},
    {"nuw", LR_TOK_NUW},
    {"inbounds", LR_TOK_INBOUNDS},
    {"nonnull", LR_TOK_NONNULL},
    {"noundef", LR_TOK_NOUNDEF},
    {"signext", LR_TOK_SIGNEXT},
    {"zeroext", LR_TOK_ZEROEXT},
    {"nocapture", LR_TOK_NOCAPTURE},
    {"readonly", LR_TOK_READONLY},
    {"writeonly", LR_TOK_WRITEONLY},
    {"global", LR_TOK_GLOBAL},
    {"constant", LR_TOK_CONSTANT},
    {"external", LR_TOK_EXTERNAL},
    {"internal", LR_TOK_INTERNAL},
    {"private", LR_TOK_PRIVATE},
    {"common", LR_TOK_COMMON},
    {"linkonce_odr", LR_TOK_LINKONCE_ODR},
    {"dso_local", LR_TOK_DSOLOCAL},
    {"unnamed_addr", LR_TOK_UNNAMED_ADDR},
    {"local_unnamed_addr", LR_TOK_LOCAL_UNNAMED_ADDR},
    {"type", LR_TOK_TYPE},
    {"opaque", LR_TOK_OPAQUE},
    {"null", LR_TOK_NULL},
    {"undef", LR_TOK_UNDEF},
    {"zeroinitializer", LR_TOK_ZEROINITIALIZER},
    {"true", LR_TOK_TRUE},
    {"false", LR_TOK_FALSE},
    {"nnan", LR_TOK_NNAN},
    {"ninf", LR_TOK_NINF},
    {"nsz", LR_TOK_NSZ},
    {"void", LR_TOK_VOID},
    {"float", LR_TOK_FLOAT},
    {"double", LR_TOK_DOUBLE},
    {"ptr", LR_TOK_PTR},
    {"eq", LR_TOK_EQ},
    {"ne", LR_TOK_NE},
    {"sgt", LR_TOK_SGT},
    {"sge", LR_TOK_SGE},
    {"slt", LR_TOK_SLT},
    {"sle", LR_TOK_SLE},
    {"ugt", LR_TOK_UGT},
    {"uge", LR_TOK_UGE},
    {"ult", LR_TOK_ULT},
    {"ule", LR_TOK_ULE},
    {"oeq", LR_TOK_OEQ},
    {"one", LR_TOK_ONE},
    {"ogt", LR_TOK_OGT},
    {"oge", LR_TOK_OGE},
    {"olt", LR_TOK_OLT},
    {"ole", LR_TOK_OLE},
    {"ord", LR_TOK_ORD},
    {"ueq", LR_TOK_UEQ},
    {"une", LR_TOK_UNE},
    {"uno", LR_TOK_UNO},
    {"x", LR_TOK_X},
    {NULL, LR_TOK_EOF}
};

static lr_tok_t lookup_keyword(const char *s, size_t len) {
    for (const keyword_t *kw = keywords; kw->name; kw++) {
        size_t klen = strlen(kw->name);
        if (klen == len && memcmp(s, kw->name, len) == 0)
            return kw->tok;
    }
    /* i1, i8, i16, i32, i64 */
    if (len >= 2 && s[0] == 'i' && isdigit((unsigned char)s[1])) {
        char buf[16];
        if (len < sizeof(buf)) {
            memcpy(buf, s + 1, len - 1);
            buf[len - 1] = '\0';
            int bits = atoi(buf);
            switch (bits) {
            case 1:  return LR_TOK_I1;
            case 8:  return LR_TOK_I8;
            case 16: return LR_TOK_I16;
            case 32: return LR_TOK_I32;
            case 64: return LR_TOK_I64;
            }
        }
    }
    return LR_TOK_EOF;
}

static lr_token_t make_token(lr_lexer_t *lex, lr_tok_t kind,
                              const char *start, size_t len) {
    lr_token_t t = {0};
    t.kind = kind;
    t.start = start;
    t.len = len;
    t.line = lex->line;
    t.col = lex->col;
    return t;
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$';
}

lr_token_t lr_lexer_next(lr_lexer_t *lex) {
    skip_whitespace_and_comments(lex);
    if (lex->pos >= lex->src_len)
        return make_token(lex, LR_TOK_EOF, lex->src + lex->pos, 0);

    const char *start = lex->src + lex->pos;
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->col;
    char c = advance(lex);
    lr_token_t tok = {0};

    switch (c) {
    case '(': tok = make_token(lex, LR_TOK_LPAREN, start, 1); break;
    case ')': tok = make_token(lex, LR_TOK_RPAREN, start, 1); break;
    case '{': tok = make_token(lex, LR_TOK_LBRACE, start, 1); break;
    case '}': tok = make_token(lex, LR_TOK_RBRACE, start, 1); break;
    case '[': tok = make_token(lex, LR_TOK_LBRACKET, start, 1); break;
    case ']': tok = make_token(lex, LR_TOK_RBRACKET, start, 1); break;
    case ',': tok = make_token(lex, LR_TOK_COMMA, start, 1); break;
    case '=': tok = make_token(lex, LR_TOK_EQUALS, start, 1); break;
    case '*': tok = make_token(lex, LR_TOK_STAR, start, 1); break;
    case ':': tok = make_token(lex, LR_TOK_COLON, start, 1); break;
    case '<': tok = make_token(lex, LR_TOK_LANGLE, start, 1); break;
    case '>': tok = make_token(lex, LR_TOK_RANGLE, start, 1); break;
    case '.':
        if (lex->pos + 1 < lex->src_len &&
            lex->src[lex->pos] == '.' && lex->src[lex->pos + 1] == '.') {
            advance(lex); advance(lex);
            tok = make_token(lex, LR_TOK_DOTDOTDOT, start, 3);
        } else if (is_ident_char(peek(lex))) {
            while (lex->pos < lex->src_len && is_ident_char(peek(lex)))
                advance(lex);
            size_t len = (size_t)(lex->src + lex->pos - start);
            tok = make_token(lex, LR_TOK_LOCAL_ID, start, len);
        } else {
            tok = make_token(lex, LR_TOK_ERROR, start, 1);
        }
        break;

    case '!': {
        while (lex->pos < lex->src_len && is_ident_char(peek(lex)))
            advance(lex);
        size_t len = (size_t)(lex->src + lex->pos - start);
        tok = make_token(lex, LR_TOK_METADATA_ID, start, len);
        break;
    }

    case '#': {
        while (lex->pos < lex->src_len && isdigit((unsigned char)peek(lex)))
            advance(lex);
        size_t len = (size_t)(lex->src + lex->pos - start);
        tok = make_token(lex, LR_TOK_ATTR_GROUP, start, len);
        break;
    }

    case '%': {
        if (peek(lex) == '"') {
            advance(lex);
            while (lex->pos < lex->src_len && peek(lex) != '"')
                advance(lex);
            advance(lex);
        } else {
            while (lex->pos < lex->src_len && is_ident_char(peek(lex)))
                advance(lex);
        }
        size_t len = (size_t)(lex->src + lex->pos - start);
        tok = make_token(lex, LR_TOK_LOCAL_ID, start, len);
        tok.start = start;
        tok.len = len;
        break;
    }

    case '@': {
        if (peek(lex) == '"') {
            advance(lex);
            while (lex->pos < lex->src_len && peek(lex) != '"')
                advance(lex);
            advance(lex);
        } else {
            while (lex->pos < lex->src_len && is_ident_char(peek(lex)))
                advance(lex);
        }
        size_t len = (size_t)(lex->src + lex->pos - start);
        tok = make_token(lex, LR_TOK_GLOBAL_ID, start, len);
        break;
    }

    case '"': {
        while (lex->pos < lex->src_len && peek(lex) != '"') {
            if (peek(lex) == '\\') advance(lex);
            advance(lex);
        }
        if (lex->pos < lex->src_len) advance(lex);
        size_t len = (size_t)(lex->src + lex->pos - start);
        tok = make_token(lex, LR_TOK_STRING_LIT, start, len);
        break;
    }

    case 'c':
        if (peek(lex) == '"') {
            advance(lex);
            while (lex->pos < lex->src_len && peek(lex) != '"') {
                if (peek(lex) == '\\') advance(lex);
                advance(lex);
            }
            if (lex->pos < lex->src_len) advance(lex);
            size_t len = (size_t)(lex->src + lex->pos - start);
            tok = make_token(lex, LR_TOK_STRING_LIT, start, len);
            break;
        }
        /* fall through to identifier handling */
        goto ident;

    default:
        if (c == '-' || isdigit((unsigned char)c)) {
            bool is_neg = (c == '-');
            if (is_neg && !isdigit((unsigned char)peek(lex))) {
                tok = make_token(lex, LR_TOK_ERROR, start, 1);
                break;
            }
            /* check for hex float: 0x... */
            if (c == '0' && (peek(lex) == 'x' || peek(lex) == 'X')) {
                advance(lex);
                while (lex->pos < lex->src_len && isxdigit((unsigned char)peek(lex)))
                    advance(lex);
                size_t len = (size_t)(lex->src + lex->pos - start);
                tok = make_token(lex, LR_TOK_FLOAT_LIT, start, len);
                /* parse hex float as raw i64 bits */
                char buf[32];
                if (len - 2 < sizeof(buf)) {
                    memcpy(buf, start + 2, len - 2);
                    buf[len - 2] = '\0';
                    uint64_t bits = strtoull(buf, NULL, 16);
                    double d;
                    memcpy(&d, &bits, sizeof(d));
                    tok.float_val = d;
                }
                break;
            }
            while (lex->pos < lex->src_len && isdigit((unsigned char)peek(lex)))
                advance(lex);
            if (peek(lex) == '.' || peek(lex) == 'e' || peek(lex) == 'E') {
                if (peek(lex) == '.') {
                    advance(lex);
                    while (lex->pos < lex->src_len && isdigit((unsigned char)peek(lex)))
                        advance(lex);
                }
                if (peek(lex) == 'e' || peek(lex) == 'E') {
                    advance(lex);
                    if (peek(lex) == '+' || peek(lex) == '-') advance(lex);
                    while (lex->pos < lex->src_len && isdigit((unsigned char)peek(lex)))
                        advance(lex);
                }
                size_t len = (size_t)(lex->src + lex->pos - start);
                tok = make_token(lex, LR_TOK_FLOAT_LIT, start, len);
                char buf[64];
                if (len < sizeof(buf)) {
                    memcpy(buf, start, len);
                    buf[len] = '\0';
                    tok.float_val = strtod(buf, NULL);
                }
            } else {
                size_t len = (size_t)(lex->src + lex->pos - start);
                tok = make_token(lex, LR_TOK_INT_LIT, start, len);
                char buf[32];
                if (len < sizeof(buf)) {
                    memcpy(buf, start, len);
                    buf[len] = '\0';
                    tok.int_val = strtoll(buf, NULL, 10);
                }
            }
            break;
        }
    ident:
        if (isalpha((unsigned char)c) || c == '_') {
            while (lex->pos < lex->src_len && is_ident_char(peek(lex)))
                advance(lex);
            size_t len = (size_t)(lex->src + lex->pos - start);
            lr_tok_t kw = lookup_keyword(start, len);
            if (kw != LR_TOK_EOF) {
                tok = make_token(lex, kw, start, len);
            } else {
                /* treat unknown bare identifiers as local ids for now */
                tok = make_token(lex, LR_TOK_LOCAL_ID, start, len);
            }
            break;
        }
        tok = make_token(lex, LR_TOK_ERROR, start, 1);
        break;
    }

    tok.line = start_line;
    tok.col = start_col;
    return tok;
}

const char *lr_tok_name(lr_tok_t kind) {
    switch (kind) {
    case LR_TOK_EOF:       return "eof";
    case LR_TOK_ERROR:     return "error";
    case LR_TOK_DEFINE:    return "define";
    case LR_TOK_DECLARE:   return "declare";
    case LR_TOK_RET:       return "ret";
    case LR_TOK_BR:        return "br";
    case LR_TOK_LABEL:     return "label";
    case LR_TOK_ADD:       return "add";
    case LR_TOK_SUB:       return "sub";
    case LR_TOK_MUL:       return "mul";
    case LR_TOK_SDIV:      return "sdiv";
    case LR_TOK_SREM:      return "srem";
    case LR_TOK_UREM:      return "urem";
    case LR_TOK_CALL:      return "call";
    case LR_TOK_GETELEMENTPTR: return "getelementptr";
    case LR_TOK_ALIGN:     return "align";
    case LR_TOK_VOID:      return "void";
    case LR_TOK_I1:        return "i1";
    case LR_TOK_I8:        return "i8";
    case LR_TOK_I16:       return "i16";
    case LR_TOK_I32:       return "i32";
    case LR_TOK_I64:       return "i64";
    case LR_TOK_FLOAT:     return "float";
    case LR_TOK_DOUBLE:    return "double";
    case LR_TOK_PTR:       return "ptr";
    case LR_TOK_LOCAL_ID:  return "local_id";
    case LR_TOK_GLOBAL_ID: return "global_id";
    case LR_TOK_INT_LIT:   return "int_lit";
    case LR_TOK_FLOAT_LIT: return "float_lit";
    case LR_TOK_STRING_LIT: return "string_lit";
    case LR_TOK_LPAREN:    return "(";
    case LR_TOK_RPAREN:    return ")";
    case LR_TOK_LBRACE:    return "{";
    case LR_TOK_RBRACE:    return "}";
    case LR_TOK_LBRACKET:  return "[";
    case LR_TOK_RBRACKET:  return "]";
    case LR_TOK_COMMA:     return ",";
    case LR_TOK_EQUALS:    return "=";
    case LR_TOK_STAR:      return "*";
    case LR_TOK_COLON:     return ":";
    case LR_TOK_LANGLE:    return "<";
    case LR_TOK_RANGLE:    return ">";
    case LR_TOK_DOTDOTDOT: return "...";
    default: return "?";
    }
}
