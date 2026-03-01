#ifndef LIRIC_LL_LEXER_H
#define LIRIC_LL_LEXER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum lr_tok {
    LR_TOK_EOF = 0,
    LR_TOK_ERROR,

    /* keywords */
    LR_TOK_DEFINE,
    LR_TOK_DECLARE,
    LR_TOK_RET,
    LR_TOK_BR,
    LR_TOK_LABEL,
    LR_TOK_ADD,
    LR_TOK_SUB,
    LR_TOK_MUL,
    LR_TOK_SDIV,
    LR_TOK_SREM,
    LR_TOK_UDIV,
    LR_TOK_UREM,
    LR_TOK_AND,
    LR_TOK_OR,
    LR_TOK_XOR,
    LR_TOK_SHL,
    LR_TOK_LSHR,
    LR_TOK_ASHR,
    LR_TOK_FADD,
    LR_TOK_FSUB,
    LR_TOK_FMUL,
    LR_TOK_FDIV,
    LR_TOK_FREM,
    LR_TOK_FNEG,
    LR_TOK_ICMP,
    LR_TOK_FCMP,
    LR_TOK_ALLOCA,
    LR_TOK_LOAD,
    LR_TOK_STORE,
    LR_TOK_GETELEMENTPTR,
    LR_TOK_CALL,
    LR_TOK_PHI,
    LR_TOK_SELECT,
    LR_TOK_SEXT,
    LR_TOK_ZEXT,
    LR_TOK_TRUNC,
    LR_TOK_BITCAST,
    LR_TOK_PTRTOINT,
    LR_TOK_INTTOPTR,
    LR_TOK_SITOFP,
    LR_TOK_UITOFP,
    LR_TOK_FPTOSI,
    LR_TOK_FPTOUI,
    LR_TOK_FPEXT,
    LR_TOK_FPTRUNC,
    LR_TOK_EXTRACTVALUE,
    LR_TOK_INSERTVALUE,
    LR_TOK_UNREACHABLE,
    LR_TOK_SWITCH,
    LR_TOK_INVOKE,
    LR_TOK_LANDINGPAD,
    LR_TOK_RESUME,
    LR_TOK_UNWIND,
    LR_TOK_CLEANUP,
    LR_TOK_CATCH,
    LR_TOK_PERSONALITY,
    LR_TOK_TO,
    LR_TOK_ALIGN,
    LR_TOK_NSW,
    LR_TOK_NUW,
    LR_TOK_INBOUNDS,
    LR_TOK_NONNULL,
    LR_TOK_NOUNDEF,
    LR_TOK_SIGNEXT,
    LR_TOK_ZEROEXT,
    LR_TOK_NOCAPTURE,
    LR_TOK_READONLY,
    LR_TOK_WRITEONLY,
    LR_TOK_GLOBAL,
    LR_TOK_CONSTANT,
    LR_TOK_EXTERNAL,
    LR_TOK_INTERNAL,
    LR_TOK_PRIVATE,
    LR_TOK_COMMON,
    LR_TOK_LINKONCE_ODR,
    LR_TOK_DSOLOCAL,
    LR_TOK_UNNAMED_ADDR,
    LR_TOK_LOCAL_UNNAMED_ADDR,
    LR_TOK_TYPE,
    LR_TOK_OPAQUE,
    LR_TOK_NULL,
    LR_TOK_UNDEF,
    LR_TOK_ZEROINITIALIZER,
    LR_TOK_TRUE,
    LR_TOK_FALSE,
    LR_TOK_NNAN,
    LR_TOK_NINF,
    LR_TOK_NSZ,

    /* icmp predicates */
    LR_TOK_EQ,
    LR_TOK_NE,
    LR_TOK_SGT,
    LR_TOK_SGE,
    LR_TOK_SLT,
    LR_TOK_SLE,
    LR_TOK_UGT,
    LR_TOK_UGE,
    LR_TOK_ULT,
    LR_TOK_ULE,

    /* fcmp predicates */
    LR_TOK_OEQ,
    LR_TOK_ONE,
    LR_TOK_OGT,
    LR_TOK_OGE,
    LR_TOK_OLT,
    LR_TOK_OLE,
    LR_TOK_ORD,
    LR_TOK_UEQ,
    LR_TOK_UNE,
    LR_TOK_UNO,

    /* types */
    LR_TOK_VOID,
    LR_TOK_I1,
    LR_TOK_I8,
    LR_TOK_I16,
    LR_TOK_I32,
    LR_TOK_I64,
    LR_TOK_FLOAT,
    LR_TOK_DOUBLE,
    LR_TOK_X86_FP80,
    LR_TOK_PTR,

    /* identifiers */
    LR_TOK_LOCAL_ID,    /* %name or %0 */
    LR_TOK_GLOBAL_ID,   /* @name */
    LR_TOK_INT_LIT,     /* 42, -7 */
    LR_TOK_FLOAT_LIT,   /* 3.14, 0x... hex float */
    LR_TOK_STRING_LIT,  /* "hello" (for metadata, etc.) */

    /* punctuation */
    LR_TOK_LPAREN,
    LR_TOK_RPAREN,
    LR_TOK_LBRACE,
    LR_TOK_RBRACE,
    LR_TOK_LBRACKET,
    LR_TOK_RBRACKET,
    LR_TOK_COMMA,
    LR_TOK_EQUALS,
    LR_TOK_STAR,
    LR_TOK_DOTDOTDOT,
    LR_TOK_COLON,
    LR_TOK_LANGLE,
    LR_TOK_RANGLE,
    LR_TOK_EXCLAIM,
    LR_TOK_X,           /* 'x' in array types like [4 x i32] */
    LR_TOK_HASH,        /* # for attribute groups */
    LR_TOK_ATTR_GROUP,  /* #42 */
    LR_TOK_METADATA_ID, /* !42 or !name */
    LR_TOK_NEWLINE,
} lr_tok_t;

typedef struct lr_token {
    lr_tok_t kind;
    const char *start;
    size_t len;
    int64_t int_val;
    double float_val;
} lr_token_t;

typedef struct lr_lexer {
    const char *src;
    size_t src_len;
    size_t pos;
} lr_lexer_t;

void lr_lexer_init(lr_lexer_t *lex, const char *src, size_t len);
lr_token_t lr_lexer_next(lr_lexer_t *lex);
const char *lr_tok_name(lr_tok_t kind);
void lr_lexer_compute_loc(const lr_lexer_t *lex, const char *pos,
                          uint32_t *out_line, uint32_t *out_col);

#endif
