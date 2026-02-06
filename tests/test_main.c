#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

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

#define RUN_TEST(fn) do { \
    tests_run++; \
    fprintf(stderr, "  %s...", #fn); \
    if (fn() == 0) { \
        tests_passed++; \
        fprintf(stderr, " ok\n"); \
    } else { \
        tests_failed++; \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* Test declarations */
int test_lexer_basic(void);
int test_lexer_types(void);
int test_lexer_identifiers(void);
int test_parser_ret_i32(void);
int test_parser_function_decl(void);
int test_parser_add(void);
int test_parser_typed_call_and_dot_label(void);
int test_parser_named_type_operand(void);
int test_codegen_ret_42(void);
int test_codegen_add(void);
int test_host_target_name(void);
int test_create_host_target(void);
int test_create_unknown_target_fails(void);
int test_non_host_target_fails(void);
int test_jit_ret_42(void);
int test_jit_add_args(void);
int test_jit_arithmetic(void);
int test_jit_icmp(void);
int test_jit_branch(void);
int test_jit_loop(void);
int test_jit_alloca_load_store(void);
int test_jit_forward_typed_call(void);
int test_e2e_ret_42(void);
int test_e2e_add_i32(void);
int test_e2e_branch(void);
int test_e2e_loop(void);

int main(void) {
    fprintf(stderr, "liric test suite\n");
    fprintf(stderr, "================\n\n");

    fprintf(stderr, "Lexer tests:\n");
    RUN_TEST(test_lexer_basic);
    RUN_TEST(test_lexer_types);
    RUN_TEST(test_lexer_identifiers);

    fprintf(stderr, "\nParser tests:\n");
    RUN_TEST(test_parser_ret_i32);
    RUN_TEST(test_parser_function_decl);
    RUN_TEST(test_parser_add);
    RUN_TEST(test_parser_typed_call_and_dot_label);
    RUN_TEST(test_parser_named_type_operand);

    fprintf(stderr, "\nCodegen tests:\n");
    RUN_TEST(test_codegen_ret_42);
    RUN_TEST(test_codegen_add);

    fprintf(stderr, "\nTarget tests:\n");
    RUN_TEST(test_host_target_name);
    RUN_TEST(test_create_host_target);
    RUN_TEST(test_create_unknown_target_fails);
    RUN_TEST(test_non_host_target_fails);

    fprintf(stderr, "\nJIT tests:\n");
    RUN_TEST(test_jit_ret_42);
    RUN_TEST(test_jit_add_args);
    RUN_TEST(test_jit_arithmetic);
    RUN_TEST(test_jit_icmp);
    RUN_TEST(test_jit_branch);
    RUN_TEST(test_jit_loop);
    RUN_TEST(test_jit_alloca_load_store);
    RUN_TEST(test_jit_forward_typed_call);

    fprintf(stderr, "\nE2E tests:\n");
    RUN_TEST(test_e2e_ret_42);
    RUN_TEST(test_e2e_add_i32);
    RUN_TEST(test_e2e_branch);
    RUN_TEST(test_e2e_loop);

    fprintf(stderr, "\n================\n");
    fprintf(stderr, "%d tests: %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
