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

static inline bool is_digit_ascii(char c) {
    unsigned char u = (unsigned char)c;
    return (unsigned)(u - (unsigned char)'0') < 10u;
}

static inline bool is_alpha_ascii(char c) {
    unsigned char u = (unsigned char)c;
    u = (unsigned char)(u | 32u);
    return (unsigned)(u - (unsigned char)'a') < 26u;
}

static inline bool is_ident_char(char c) {
    return is_alpha_ascii(c) || is_digit_ascii(c) || c == '_' || c == '.' || c == '$';
}

static void skip_whitespace_and_comments(lr_lexer_t *lex) {
    const char *src = lex->src;
    size_t pos = lex->pos;
    size_t n = lex->src_len;
    uint32_t line = lex->line;
    uint32_t col = lex->col;

    while (pos < n) {
        char c = src[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pos++;
            if (c == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        } else if (c == ';') {
            pos++;
            col++;
            while (pos < n && src[pos] != '\n') {
                pos++;
                col++;
            }
        } else {
            break;
        }
    }

    lex->pos = pos;
    lex->line = line;
    lex->col = col;
}

typedef struct { const char *name; lr_tok_t tok; } keyword_t;

static const keyword_t keywords[] __attribute__((unused)) = {
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

static uint32_t keyword_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static lr_tok_t lookup_keyword(const char *s, size_t len) {
    if (len == 0)
        return LR_TOK_EOF;

    /* fast path for i1/i8/i16/i32/i64 */
    if (s[0] == 'i') {
        if (len == 2) {
            if (s[1] == '1') return LR_TOK_I1;
            if (s[1] == '8') return LR_TOK_I8;
        } else if (len == 3 && s[1] >= '0' && s[1] <= '9' &&
                   s[2] >= '0' && s[2] <= '9') {
            if (s[1] == '1' && s[2] == '6') return LR_TOK_I16;
            if (s[1] == '3' && s[2] == '2') return LR_TOK_I32;
            if (s[1] == '6' && s[2] == '4') return LR_TOK_I64;
        }
    }

    switch (keyword_hash(s, len)) {
    case 0x01f7e39fu:
        if (len == 7 && memcmp(s, "signext", 7) == 0) return LR_TOK_SIGNEXT;
        break;
    case 0x0684099cu:
        if (len == 4 && memcmp(s, "urem", 4) == 0) return LR_TOK_UREM;
        break;
    case 0x0691ea25u:
        if (len == 8 && memcmp(s, "constant", 8) == 0) return LR_TOK_CONSTANT;
        break;
    case 0x0b069958u:
        if (len == 5 && memcmp(s, "false", 5) == 0) return LR_TOK_FALSE;
        break;
    case 0x0f29c2a6u:
        if (len == 3 && memcmp(s, "and", 3) == 0) return LR_TOK_AND;
        break;
    case 0x11c2662du:
        if (len == 6 && memcmp(s, "select", 6) == 0) return LR_TOK_SELECT;
        break;
    case 0x13266a9cu:
        if (len == 4 && memcmp(s, "ninf", 4) == 0) return LR_TOK_NINF;
        break;
    case 0x186c5d43u:
        if (len == 3 && memcmp(s, "nsw", 3) == 0) return LR_TOK_NSW;
        break;
    case 0x1c362229u:
        if (len == 4 && memcmp(s, "fsub", 4) == 0) return LR_TOK_FSUB;
        break;
    case 0x1cb0ec77u:
        if (len == 4 && memcmp(s, "ashr", 4) == 0) return LR_TOK_ASHR;
        break;
    case 0x1dff06aeu:
        if (len == 6 && memcmp(s, "global", 6) == 0) return LR_TOK_GLOBAL;
        break;
    case 0x236c6e94u:
        if (len == 3 && memcmp(s, "nsz", 3) == 0) return LR_TOK_NSZ;
        break;
    case 0x287286a1u:
        if (len == 3 && memcmp(s, "nuw", 3) == 0) return LR_TOK_NUW;
        break;
    case 0x2c9151f9u:
        if (len == 7 && memcmp(s, "nonnull", 7) == 0) return LR_TOK_NONNULL;
        break;
    case 0x30dbb8a6u:
        if (len == 4 && memcmp(s, "srem", 4) == 0) return LR_TOK_SREM;
        break;
    case 0x30f467acu:
        if (len == 3 && memcmp(s, "ret", 3) == 0) return LR_TOK_RET;
        break;
    case 0x331d748au:
        if (len == 8 && memcmp(s, "external", 8) == 0) return LR_TOK_EXTERNAL;
        break;
    case 0x33952f14u:
        if (len == 4 && memcmp(s, "fdiv", 4) == 0) return LR_TOK_FDIV;
        break;
    case 0x34efc59fu:
        if (len == 7 && memcmp(s, "bitcast", 7) == 0) return LR_TOK_BITCAST;
        break;
    case 0x36cff9c0u:
        if (len == 3 && memcmp(s, "ult", 3) == 0) return LR_TOK_ULT;
        break;
    case 0x38c330f3u:
        if (len == 3 && memcmp(s, "ugt", 3) == 0) return LR_TOK_UGT;
        break;
    case 0x39262605u:
        if (len == 7 && memcmp(s, "declare", 7) == 0) return LR_TOK_DECLARE;
        break;
    case 0x3b391274u:
        if (len == 3 && memcmp(s, "add", 3) == 0) return LR_TOK_ADD;
        break;
    case 0x41437d99u:
        if (len == 11 && memcmp(s, "insertvalue", 11) == 0) return LR_TOK_INSERTVALUE;
        break;
    case 0x42454824u:
        if (len == 2 && memcmp(s, "to", 2) == 0) return LR_TOK_TO;
        break;
    case 0x441a6a43u:
        if (len == 2 && memcmp(s, "eq", 2) == 0) return LR_TOK_EQ;
        break;
    case 0x47c34890u:
        if (len == 3 && memcmp(s, "uge", 3) == 0) return LR_TOK_UGE;
        break;
    case 0x47d01483u:
        if (len == 3 && memcmp(s, "ule", 3) == 0) return LR_TOK_ULE;
        break;
    case 0x48b5725fu:
        if (len == 4 && memcmp(s, "void", 4) == 0) return LR_TOK_VOID;
        break;
    case 0x4c75e965u:
        if (len == 4 && memcmp(s, "fmul", 4) == 0) return LR_TOK_FMUL;
        break;
    case 0x4d5b0474u:
        if (len == 6 && memcmp(s, "common", 6) == 0) return LR_TOK_COMMON;
        break;
    case 0x4db211e5u:
        if (len == 4 && memcmp(s, "true", 4) == 0) return LR_TOK_TRUE;
        break;
    case 0x4f2bc4b5u:
        if (len == 2 && memcmp(s, "br", 2) == 0) return LR_TOK_BR;
        break;
    case 0x4fbed7fau:
        if (len == 3 && memcmp(s, "ueq", 3) == 0) return LR_TOK_UEQ;
        break;
    case 0x4ff85ef6u:
        if (len == 4 && memcmp(s, "lshr", 4) == 0) return LR_TOK_LSHR;
        break;
    case 0x5127f14du:
        if (len == 4 && memcmp(s, "type", 4) == 0) return LR_TOK_TYPE;
        break;
    case 0x51d4a16fu:
        if (len == 3 && memcmp(s, "uno", 3) == 0) return LR_TOK_UNO;
        break;
    case 0x5244aa84u:
        if (len == 3 && memcmp(s, "phi", 3) == 0) return LR_TOK_PHI;
        break;
    case 0x55764c09u:
        if (len == 3 && memcmp(s, "ptr", 3) == 0) return LR_TOK_PTR;
        break;
    case 0x5836603cu:
        if (len == 2 && memcmp(s, "ne", 2) == 0) return LR_TOK_NE;
        break;
    case 0x58b04806u:
        if (len == 5 && memcmp(s, "fpext", 5) == 0) return LR_TOK_FPEXT;
        break;
    case 0x5bd4b12du:
        if (len == 3 && memcmp(s, "une", 3) == 0) return LR_TOK_UNE;
        break;
    case 0x5cca1c55u:
        if (len == 6 && memcmp(s, "alloca", 6) == 0) return LR_TOK_ALLOCA;
        break;
    case 0x5d342984u:
        if (len == 2 && memcmp(s, "or", 2) == 0) return LR_TOK_OR;
        break;
    case 0x602c63deu:
        if (len == 5 && memcmp(s, "align", 5) == 0) return LR_TOK_ALIGN;
        break;
    case 0x62cb0d0cu:
        if (len == 7 && memcmp(s, "private", 7) == 0) return LR_TOK_PRIVATE;
        break;
    case 0x668cbeb0u:
        if (len == 7 && memcmp(s, "noundef", 7) == 0) return LR_TOK_NOUNDEF;
        break;
    case 0x668d4269u:
        if (len == 4 && memcmp(s, "sext", 4) == 0) return LR_TOK_SEXT;
        break;
    case 0x670195b6u:
        if (len == 6 && memcmp(s, "sitofp", 6) == 0) return LR_TOK_SITOFP;
        break;
    case 0x6a9f8552u:
        if (len == 6 && memcmp(s, "define", 6) == 0) return LR_TOK_DEFINE;
        break;
    case 0x6f15601eu:
        if (len == 6 && memcmp(s, "opaque", 6) == 0) return LR_TOK_OPAQUE;
        break;
    case 0x705624fcu:
        if (len == 4 && memcmp(s, "zext", 4) == 0) return LR_TOK_ZEXT;
        break;
    case 0x75191b51u:
        if (len == 5 && memcmp(s, "undef", 5) == 0) return LR_TOK_UNDEF;
        break;
    case 0x77074ba4u:
        if (len == 4 && memcmp(s, "null", 4) == 0) return LR_TOK_NULL;
        break;
    case 0x7f6a0e3fu:
        if (len == 12 && memcmp(s, "extractvalue", 12) == 0) return LR_TOK_EXTRACTVALUE;
        break;
    case 0x7f795119u:
        if (len == 8 && memcmp(s, "ptrtoint", 8) == 0) return LR_TOK_PTRTOINT;
        break;
    case 0x91fa8f16u:
        if (len == 9 && memcmp(s, "writeonly", 9) == 0) return LR_TOK_WRITEONLY;
        break;
    case 0x991deba0u:
        if (len == 3 && memcmp(s, "ord", 3) == 0) return LR_TOK_ORD;
        break;
    case 0x9a796d00u:
        if (len == 8 && memcmp(s, "internal", 8) == 0) return LR_TOK_INTERNAL;
        break;
    case 0xa02c414du:
        if (len == 13 && memcmp(s, "getelementptr", 13) == 0) return LR_TOK_GETELEMENTPTR;
        break;
    case 0xa0eb0f08u:
        if (len == 6 && memcmp(s, "double", 6) == 0) return LR_TOK_DOUBLE;
        break;
    case 0xa642d0f0u:
        if (len == 3 && memcmp(s, "oeq", 3) == 0) return LR_TOK_OEQ;
        break;
    case 0xa6c45d85u:
        if (len == 5 && memcmp(s, "float", 5) == 0) return LR_TOK_FLOAT;
        break;
    case 0xa7105b55u:
        if (len == 4 && memcmp(s, "fneg", 4) == 0) return LR_TOK_FNEG;
        break;
    case 0xaaedf37fu:
        if (len == 8 && memcmp(s, "inbounds", 8) == 0) return LR_TOK_INBOUNDS;
        break;
    case 0xad2b82a6u:
        if (len == 3 && memcmp(s, "olt", 3) == 0) return LR_TOK_OLT;
        break;
    case 0xb3f184a9u:
        if (len == 4 && memcmp(s, "call", 4) == 0) return LR_TOK_CALL;
        break;
    case 0xb6b438feu:
        if (len == 7 && memcmp(s, "zeroext", 7) == 0) return LR_TOK_ZEROEXT;
        break;
    case 0xb736d533u:
        if (len == 18 && memcmp(s, "local_unnamed_addr", 18) == 0) return LR_TOK_LOCAL_UNNAMED_ADDR;
        break;
    case 0xba2719efu:
        if (len == 3 && memcmp(s, "one", 3) == 0) return LR_TOK_ONE;
        break;
    case 0xbe2b9d69u:
        if (len == 3 && memcmp(s, "ole", 3) == 0) return LR_TOK_OLE;
        break;
    case 0xbf3ce81du:
        if (len == 3 && memcmp(s, "ogt", 3) == 0) return LR_TOK_OGT;
        break;
    case 0xc7649392u:
        if (len == 6 && memcmp(s, "fptosi", 6) == 0) return LR_TOK_FPTOSI;
        break;
    case 0xc7e7bc2eu:
        if (len == 5 && memcmp(s, "store", 5) == 0) return LR_TOK_STORE;
        break;
    case 0xc93854ddu:
        if (len == 3 && memcmp(s, "sle", 3) == 0) return LR_TOK_SLE;
        break;
    case 0xcc6bdb7eu:
        if (len == 3 && memcmp(s, "xor", 3) == 0) return LR_TOK_XOR;
        break;
    case 0xce0beff7u:
        if (len == 8 && memcmp(s, "readonly", 8) == 0) return LR_TOK_READONLY;
        break;
    case 0xce3cffbau:
        if (len == 3 && memcmp(s, "oge", 3) == 0) return LR_TOK_OGE;
        break;
    case 0xd522b60du:
        if (len == 9 && memcmp(s, "dso_local", 9) == 0) return LR_TOK_DSOLOCAL;
        break;
    case 0xd55e61e5u:
        if (len == 5 && memcmp(s, "trunc", 5) == 0) return LR_TOK_TRUNC;
        break;
    case 0xd809e3a3u:
        if (len == 12 && memcmp(s, "unnamed_addr", 12) == 0) return LR_TOK_UNNAMED_ADDR;
        break;
    case 0xd8386c7au:
        if (len == 3 && memcmp(s, "slt", 3) == 0) return LR_TOK_SLT;
        break;
    case 0xd83b82b8u:
        if (len == 4 && memcmp(s, "icmp", 4) == 0) return LR_TOK_ICMP;
        break;
    case 0xd89e21f1u:
        if (len == 11 && memcmp(s, "unreachable", 11) == 0) return LR_TOK_UNREACHABLE;
        break;
    case 0xda211651u:
        if (len == 3 && memcmp(s, "sgt", 3) == 0) return LR_TOK_SGT;
        break;
    case 0xdc4e3915u:
        if (len == 3 && memcmp(s, "sub", 3) == 0) return LR_TOK_SUB;
        break;
    case 0xe02debb6u:
        if (len == 3 && memcmp(s, "shl", 3) == 0) return LR_TOK_SHL;
        break;
    case 0xe0c77861u:
        if (len == 15 && memcmp(s, "zeroinitializer", 15) == 0) return LR_TOK_ZEROINITIALIZER;
        break;
    case 0xe2f81043u:
        if (len == 7 && memcmp(s, "fptrunc", 7) == 0) return LR_TOK_FPTRUNC;
        break;
    case 0xe60759e9u:
        if (len == 4 && memcmp(s, "load", 4) == 0) return LR_TOK_LOAD;
        break;
    case 0xe81fa798u:
        if (len == 9 && memcmp(s, "nocapture", 9) == 0) return LR_TOK_NOCAPTURE;
        break;
    case 0xe9212deeu:
        if (len == 3 && memcmp(s, "sge", 3) == 0) return LR_TOK_SGE;
        break;
    case 0xe9f1dd8bu:
        if (len == 4 && memcmp(s, "fcmp", 4) == 0) return LR_TOK_FCMP;
        break;
    case 0xeb84ed81u:
        if (len == 3 && memcmp(s, "mul", 3) == 0) return LR_TOK_MUL;
        break;
    case 0xf06d8280u:
        if (len == 4 && memcmp(s, "fadd", 4) == 0) return LR_TOK_FADD;
        break;
    case 0xf18b906cu:
        if (len == 12 && memcmp(s, "linkonce_odr", 12) == 0) return LR_TOK_LINKONCE_ODR;
        break;
    case 0xf577d25eu:
        if (len == 4 && memcmp(s, "nnan", 4) == 0) return LR_TOK_NNAN;
        break;
    case 0xf69717fdu:
        if (len == 5 && memcmp(s, "label", 5) == 0) return LR_TOK_LABEL;
        break;
    case 0xf7c31c4fu:
        if (len == 8 && memcmp(s, "inttoptr", 8) == 0) return LR_TOK_INTTOPTR;
        break;
    case 0xf820d675u:
        if (len == 4 && memcmp(s, "sdiv", 4) == 0) return LR_TOK_SDIV;
        break;
    case 0xfd0c5087u:
        if (len == 1 && memcmp(s, "x", 1) == 0) return LR_TOK_X;
        break;
    default:
        break;
    }
    return LR_TOK_EOF;
}
static lr_token_t make_token(lr_tok_t kind, const char *start, size_t len,
                             uint32_t line, uint32_t col) {
    lr_token_t t = {0};
    t.kind = kind;
    t.start = start;
    t.len = len;
    t.line = line;
    t.col = col;
    return t;
}

lr_token_t lr_lexer_next(lr_lexer_t *lex) {
    skip_whitespace_and_comments(lex);
    if (lex->pos >= lex->src_len)
        return make_token(LR_TOK_EOF, lex->src + lex->pos, 0, lex->line, lex->col);

    const char *src = lex->src;
    size_t n = lex->src_len;
    size_t pos = lex->pos;
    uint32_t line = lex->line;
    uint32_t col = lex->col;
    size_t start_pos = pos;
    const char *start = src + start_pos;
    uint32_t start_line = line;
    uint32_t start_col = col;
    char c = src[pos++];
    col++;
    lr_token_t tok = {0};

    switch (c) {
    case '(':
        tok = make_token(LR_TOK_LPAREN, start, 1, start_line, start_col);
        break;
    case ')':
        tok = make_token(LR_TOK_RPAREN, start, 1, start_line, start_col);
        break;
    case '{':
        tok = make_token(LR_TOK_LBRACE, start, 1, start_line, start_col);
        break;
    case '}':
        tok = make_token(LR_TOK_RBRACE, start, 1, start_line, start_col);
        break;
    case '[':
        tok = make_token(LR_TOK_LBRACKET, start, 1, start_line, start_col);
        break;
    case ']':
        tok = make_token(LR_TOK_RBRACKET, start, 1, start_line, start_col);
        break;
    case ',':
        tok = make_token(LR_TOK_COMMA, start, 1, start_line, start_col);
        break;
    case '=':
        tok = make_token(LR_TOK_EQUALS, start, 1, start_line, start_col);
        break;
    case '*':
        tok = make_token(LR_TOK_STAR, start, 1, start_line, start_col);
        break;
    case ':':
        tok = make_token(LR_TOK_COLON, start, 1, start_line, start_col);
        break;
    case '<':
        tok = make_token(LR_TOK_LANGLE, start, 1, start_line, start_col);
        break;
    case '>':
        tok = make_token(LR_TOK_RANGLE, start, 1, start_line, start_col);
        break;
    case '.':
        if (pos + 1 < n && src[pos] == '.' && src[pos + 1] == '.') {
            pos += 2;
            col += 2;
            tok = make_token(LR_TOK_DOTDOTDOT, start, 3, start_line, start_col);
        } else if (pos < n && is_ident_char(src[pos])) {
            size_t run = pos;
            while (run < n && is_ident_char(src[run])) run++;
            col += (uint32_t)(run - pos);
            pos = run;
            tok = make_token(LR_TOK_LOCAL_ID, start, pos - start_pos, start_line, start_col);
        } else {
            tok = make_token(LR_TOK_ERROR, start, 1, start_line, start_col);
        }
        break;

    case '!': {
        size_t run = pos;
        while (run < n && is_ident_char(src[run])) run++;
        col += (uint32_t)(run - pos);
        pos = run;
        tok = make_token(LR_TOK_METADATA_ID, start, pos - start_pos, start_line, start_col);
        break;
    }

    case '#': {
        size_t run = pos;
        while (run < n && is_digit_ascii(src[run])) run++;
        col += (uint32_t)(run - pos);
        pos = run;
        tok = make_token(LR_TOK_ATTR_GROUP, start, pos - start_pos, start_line, start_col);
        break;
    }

    case '%':
    case '@': {
        if (pos < n && src[pos] == '"') {
            pos++;
            col++;
            while (pos < n && src[pos] != '"') {
                if (src[pos] == '\\' && pos + 1 < n) {
                    pos++;
                    col++;
                }
                if (src[pos] == '\n') {
                    pos++;
                    line++;
                    col = 1;
                } else {
                    pos++;
                    col++;
                }
            }
            if (pos < n) {
                pos++;
                col++;
            }
        } else {
            size_t run = pos;
            while (run < n && is_ident_char(src[run])) run++;
            col += (uint32_t)(run - pos);
            pos = run;
        }
        tok = make_token(c == '%' ? LR_TOK_LOCAL_ID : LR_TOK_GLOBAL_ID,
                         start, pos - start_pos, start_line, start_col);
        break;
    }

    case '"':
    case 'c': {
        if (c == 'c' && !(pos < n && src[pos] == '"')) {
            goto ident;
        }
        if (c == 'c') {
            pos++;
            col++;
        }
        while (pos < n && src[pos] != '"') {
            if (src[pos] == '\\' && pos + 1 < n) {
                pos++;
                col++;
            }
            if (src[pos] == '\n') {
                pos++;
                line++;
                col = 1;
            } else {
                pos++;
                col++;
            }
        }
        if (pos < n) {
            pos++;
            col++;
        }
        tok = make_token(LR_TOK_STRING_LIT, start, pos - start_pos, start_line, start_col);
        break;
    }

    default:
        if (c == '-' || is_digit_ascii(c)) {
            bool is_neg = (c == '-');
            if (is_neg && !(pos < n && is_digit_ascii(src[pos]))) {
                tok = make_token(LR_TOK_ERROR, start, 1, start_line, start_col);
                break;
            }

            if (c == '0' && pos < n && (src[pos] == 'x' || src[pos] == 'X')) {
                size_t run = pos + 1;
                while (run < n && isxdigit((unsigned char)src[run])) run++;
                col += (uint32_t)(run - pos);
                pos = run;
                tok = make_token(LR_TOK_FLOAT_LIT, start, pos - start_pos, start_line, start_col);
                {
                    size_t len = tok.len;
                    char buf[32];
                    if (len >= 2 && len - 2 < sizeof(buf)) {
                        memcpy(buf, start + 2, len - 2);
                        buf[len - 2] = '\0';
                        uint64_t bits = strtoull(buf, NULL, 16);
                        double d;
                        memcpy(&d, &bits, sizeof(d));
                        tok.float_val = d;
                    }
                }
                break;
            }

            {
                size_t run = pos;
                while (run < n && is_digit_ascii(src[run])) run++;
                col += (uint32_t)(run - pos);
                pos = run;
            }

            if (pos < n && (src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E')) {
                if (pos < n && src[pos] == '.') {
                    pos++;
                    col++;
                    while (pos < n && is_digit_ascii(src[pos])) {
                        pos++;
                        col++;
                    }
                }
                if (pos < n && (src[pos] == 'e' || src[pos] == 'E')) {
                    pos++;
                    col++;
                    if (pos < n && (src[pos] == '+' || src[pos] == '-')) {
                        pos++;
                        col++;
                    }
                    while (pos < n && is_digit_ascii(src[pos])) {
                        pos++;
                        col++;
                    }
                }
                tok = make_token(LR_TOK_FLOAT_LIT, start, pos - start_pos, start_line, start_col);
                {
                    size_t len = tok.len;
                    char buf[64];
                    if (len < sizeof(buf)) {
                        memcpy(buf, start, len);
                        buf[len] = '\0';
                        tok.float_val = strtod(buf, NULL);
                    }
                }
            } else {
                tok = make_token(LR_TOK_INT_LIT, start, pos - start_pos, start_line, start_col);
                {
                    size_t len = tok.len;
                    char buf[32];
                    if (len < sizeof(buf)) {
                        memcpy(buf, start, len);
                        buf[len] = '\0';
                        tok.int_val = strtoll(buf, NULL, 10);
                    }
                }
            }
            break;
        }
ident:
        if (is_alpha_ascii(c) || c == '_') {
            size_t run = pos;
            while (run < n && is_ident_char(src[run])) run++;
            col += (uint32_t)(run - pos);
            pos = run;
            {
                size_t len = pos - start_pos;
                lr_tok_t kw = lookup_keyword(start, len);
                tok = make_token(kw != LR_TOK_EOF ? kw : LR_TOK_LOCAL_ID,
                                 start, len, start_line, start_col);
            }
            break;
        }
        tok = make_token(LR_TOK_ERROR, start, 1, start_line, start_col);
        break;
    }

    lex->pos = pos;
    lex->line = line;
    lex->col = col;
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
