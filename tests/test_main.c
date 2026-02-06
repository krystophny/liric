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
int test_parser_decl_with_modern_param_attrs(void);
int test_parser_store_with_const_gep_operand(void);
int test_parser_call_arg_with_align_attr(void);
int test_parser_store_with_struct_constant(void);
int test_parser_urem_instruction(void);
int test_parser_canonical_phi_pairs(void);
int test_parser_select_with_ptr_operands(void);
int test_codegen_ret_42(void);
int test_codegen_add(void);
int test_host_target_name(void);
int test_create_host_target(void);
int test_create_unknown_target_fails(void);
int test_non_host_target_fails(void);
int test_load_missing_runtime_library_fails(void);
int test_jit_ret_42(void);
int test_jit_add_args(void);
int test_jit_arithmetic(void);
int test_jit_icmp(void);
int test_jit_branch(void);
int test_jit_loop(void);
int test_jit_alloca_load_store(void);
int test_jit_forward_typed_call(void);
int test_jit_fadd_double_bits(void);
int test_jit_fmul_float_bits(void);
int test_jit_phi_select_nested(void);
int test_jit_phi_select_loop_carried(void);
int test_jit_internal_global_load_store(void);
int test_jit_internal_global_address_relocation(void);
int test_jit_external_call_abs(void);
int test_jit_varargs_printf_call(void);
int test_e2e_ret_42(void);
int test_e2e_add_i32(void);
int test_e2e_branch(void);
int test_e2e_loop(void);
int test_wasm_leb128_u32(void);
int test_wasm_leb128_i32(void);
int test_wasm_leb128_i64(void);
int test_wasm_decode_minimal(void);
int test_wasm_decode_add(void);
int test_wasm_decode_invalid_magic(void);
int test_wasm_ir_ret_42(void);
int test_wasm_ir_add_args(void);
int test_wasm_jit_ret_42(void);
int test_wasm_jit_add_args(void);
int test_wasm_jit_branch(void);
int test_wasm_jit_loop(void);
int test_wasm_jit_call(void);

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
    RUN_TEST(test_parser_decl_with_modern_param_attrs);
    RUN_TEST(test_parser_store_with_const_gep_operand);
    RUN_TEST(test_parser_call_arg_with_align_attr);
    RUN_TEST(test_parser_store_with_struct_constant);
    RUN_TEST(test_parser_urem_instruction);
    RUN_TEST(test_parser_canonical_phi_pairs);
    RUN_TEST(test_parser_select_with_ptr_operands);

    fprintf(stderr, "\nCodegen tests:\n");
    RUN_TEST(test_codegen_ret_42);
    RUN_TEST(test_codegen_add);

    fprintf(stderr, "\nTarget tests:\n");
    RUN_TEST(test_host_target_name);
    RUN_TEST(test_create_host_target);
    RUN_TEST(test_create_unknown_target_fails);
    RUN_TEST(test_non_host_target_fails);
    RUN_TEST(test_load_missing_runtime_library_fails);

    fprintf(stderr, "\nJIT tests:\n");
    RUN_TEST(test_jit_ret_42);
    RUN_TEST(test_jit_add_args);
    RUN_TEST(test_jit_arithmetic);
    RUN_TEST(test_jit_icmp);
    RUN_TEST(test_jit_branch);
    RUN_TEST(test_jit_loop);
    RUN_TEST(test_jit_alloca_load_store);
    RUN_TEST(test_jit_forward_typed_call);
    RUN_TEST(test_jit_fadd_double_bits);
    RUN_TEST(test_jit_fmul_float_bits);
    RUN_TEST(test_jit_phi_select_nested);
    RUN_TEST(test_jit_phi_select_loop_carried);
    RUN_TEST(test_jit_internal_global_load_store);
    RUN_TEST(test_jit_internal_global_address_relocation);
    RUN_TEST(test_jit_external_call_abs);
    RUN_TEST(test_jit_varargs_printf_call);

    fprintf(stderr, "\nE2E tests:\n");
    RUN_TEST(test_e2e_ret_42);
    RUN_TEST(test_e2e_add_i32);
    RUN_TEST(test_e2e_branch);
    RUN_TEST(test_e2e_loop);

    fprintf(stderr, "\nWASM LEB128 tests:\n");
    RUN_TEST(test_wasm_leb128_u32);
    RUN_TEST(test_wasm_leb128_i32);
    RUN_TEST(test_wasm_leb128_i64);

    fprintf(stderr, "\nWASM Decoder tests:\n");
    RUN_TEST(test_wasm_decode_minimal);
    RUN_TEST(test_wasm_decode_add);
    RUN_TEST(test_wasm_decode_invalid_magic);

    fprintf(stderr, "\nWASM IR tests:\n");
    RUN_TEST(test_wasm_ir_ret_42);
    RUN_TEST(test_wasm_ir_add_args);

    fprintf(stderr, "\nWASM JIT tests:\n");
    RUN_TEST(test_wasm_jit_ret_42);
    RUN_TEST(test_wasm_jit_add_args);
    RUN_TEST(test_wasm_jit_branch);
    RUN_TEST(test_wasm_jit_loop);
    RUN_TEST(test_wasm_jit_call);

    fprintf(stderr, "\n================\n");
    fprintf(stderr, "%d tests: %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
