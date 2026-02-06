#include "../src/ll_lexer.h"
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s: got %lld, expected %lld (line %d)\n", \
                msg, _a, _b, __LINE__); \
        return 1; \
    } \
} while (0)

int test_lexer_basic(void) {
    const char *src = "define i32 @f() {\nentry:\n  ret i32 42\n}";
    lr_lexer_t lex;
    lr_lexer_init(&lex, src, strlen(src));

    lr_token_t t;
    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_DEFINE, "first token is define");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_I32, "second token is i32");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_GLOBAL_ID, "third token is global id");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_LPAREN, "fourth token is (");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_RPAREN, "fifth token is )");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_LBRACE, "sixth token is {");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_LOCAL_ID, "label token");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_COLON, "colon after label");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_RET, "ret keyword");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_I32, "i32 after ret");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_INT_LIT, "42 literal");
    TEST_ASSERT_EQ(t.int_val, 42, "42 value");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_RBRACE, "closing brace");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_EOF, "eof");

    return 0;
}

int test_lexer_types(void) {
    const char *src = "void i1 i8 i16 i32 i64 float double ptr";
    lr_lexer_t lex;
    lr_lexer_init(&lex, src, strlen(src));

    lr_tok_t expected[] = {
        LR_TOK_VOID, LR_TOK_I1, LR_TOK_I8, LR_TOK_I16,
        LR_TOK_I32, LR_TOK_I64, LR_TOK_FLOAT, LR_TOK_DOUBLE, LR_TOK_PTR
    };

    for (int i = 0; i < 9; i++) {
        lr_token_t t = lr_lexer_next(&lex);
        TEST_ASSERT_EQ(t.kind, expected[i], "type token");
    }

    return 0;
}

int test_lexer_identifiers(void) {
    const char *src = "%x @global %\"quoted name\" @\"quoted.global\"";
    lr_lexer_t lex;
    lr_lexer_init(&lex, src, strlen(src));

    lr_token_t t;
    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_LOCAL_ID, "local id %x");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_GLOBAL_ID, "global id @global");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_LOCAL_ID, "quoted local id");

    t = lr_lexer_next(&lex);
    TEST_ASSERT_EQ(t.kind, LR_TOK_GLOBAL_ID, "quoted global id");

    return 0;
}
