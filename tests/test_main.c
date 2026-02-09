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
int test_parser_typed_pointer_decl_params(void);
int test_parser_add(void);
int test_parser_typed_call_and_dot_label(void);
int test_parser_named_type_operand(void);
int test_parser_forward_named_type_by_value(void);
int test_parser_gep_runtime_index_canonicalized_i64(void);
int test_parser_decl_with_modern_param_attrs(void);
int test_parser_store_with_const_gep_operand(void);
int test_parser_call_arg_with_align_attr(void);
int test_parser_store_with_struct_constant(void);
int test_parser_store_packed_struct_float_pair(void);
int test_parser_store_packed_struct_double_pair(void);
int test_parser_urem_instruction(void);
int test_parser_canonical_phi_pairs(void);
int test_parser_select_with_ptr_operands(void);
int test_parser_bitcast_const_expr_operand(void);
int test_parser_function_pointer_type(void);
int test_parser_quoted_label_names(void);
int test_parser_boolean_literals(void);
int test_parser_named_params_no_collision(void);
int test_parser_cast_expr_in_aggregate_init(void);
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
int test_jit_alloca_many_static_slots(void);
int test_jit_forward_typed_call(void);
int test_jit_forward_call_chain(void);
int test_jit_self_recursive_call(void);
int test_jit_fadd_double_bits(void);
int test_jit_fmul_float_bits(void);
int test_jit_phi_select_nested(void);
int test_jit_phi_select_loop_carried(void);
int test_jit_internal_global_load_store(void);
int test_jit_internal_global_address_relocation(void);
int test_jit_external_call_abs(void);
int test_jit_varargs_printf_call(void);
int test_jit_varargs_printf_double_call(void);
int test_jit_const_gep_vtable_function_ptr(void);
int test_jit_llvm_intrinsic_fabs_f32(void);
int test_jit_llvm_intrinsic_memcpy_memset(void);
int test_jit_gep_struct_field(void);
int test_jit_gep_array_index(void);
int test_jit_gep_negative_i32_index(void);
int test_jit_global_string_constant(void);
int test_jit_global_struct_ptr_relocation(void);
int test_jit_global_struct_integer_init(void);
int test_jit_aggregate_load_store_copy(void);
int test_jit_call_stack_args(void);
int test_jit_call_many_stack_args(void);
int test_jit_fsub_double(void);
int test_jit_fdiv_double(void);
int test_jit_fneg_double(void);
int test_jit_sitofp_i64_f64(void);
int test_jit_fptosi_f64_i64(void);
int test_jit_fpext_f32_f64(void);
int test_jit_fptrunc_f64_f32(void);
int test_jit_fcmp_oeq(void);
int test_jit_fp_arithmetic_chain(void);
int test_jit_insert_extractvalue_struct_fields(void);
int test_jit_late_frame_patch_and_phi_slots(void);
int test_jit_packed_struct_float_constant(void);
int test_jit_packed_struct_double_constant(void);
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
int test_builder_ret_42(void);
int test_builder_add_args(void);
int test_builder_arithmetic(void);
int test_builder_icmp_branch(void);
int test_builder_loop_phi(void);
int test_builder_alloca_load_store(void);
int test_builder_gep_runtime_index_canonicalized_i64(void);
int test_builder_call(void);
int test_builder_select(void);
int test_builder_roundtrip(void);
#if !defined(__APPLE__)
int test_objfile_elf_header(void);
int test_objfile_elf_symbols(void);
int test_objfile_elf_call_relocation(void);
int test_objfile_elf_readelf_validates(void);
#else
int test_objfile_macho_header(void);
#endif

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
    RUN_TEST(test_parser_typed_pointer_decl_params);
    RUN_TEST(test_parser_add);
    RUN_TEST(test_parser_typed_call_and_dot_label);
    RUN_TEST(test_parser_named_type_operand);
    RUN_TEST(test_parser_forward_named_type_by_value);
    RUN_TEST(test_parser_gep_runtime_index_canonicalized_i64);
    RUN_TEST(test_parser_decl_with_modern_param_attrs);
    RUN_TEST(test_parser_store_with_const_gep_operand);
    RUN_TEST(test_parser_call_arg_with_align_attr);
    RUN_TEST(test_parser_store_with_struct_constant);
    RUN_TEST(test_parser_store_packed_struct_float_pair);
    RUN_TEST(test_parser_store_packed_struct_double_pair);
    RUN_TEST(test_parser_urem_instruction);
    RUN_TEST(test_parser_canonical_phi_pairs);
    RUN_TEST(test_parser_select_with_ptr_operands);
    RUN_TEST(test_parser_bitcast_const_expr_operand);
    RUN_TEST(test_parser_function_pointer_type);
    RUN_TEST(test_parser_quoted_label_names);
    RUN_TEST(test_parser_boolean_literals);
    RUN_TEST(test_parser_named_params_no_collision);
    RUN_TEST(test_parser_cast_expr_in_aggregate_init);

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
    RUN_TEST(test_jit_alloca_many_static_slots);
    RUN_TEST(test_jit_forward_typed_call);
    RUN_TEST(test_jit_forward_call_chain);
    RUN_TEST(test_jit_self_recursive_call);
    RUN_TEST(test_jit_fadd_double_bits);
    RUN_TEST(test_jit_fmul_float_bits);
    RUN_TEST(test_jit_phi_select_nested);
    RUN_TEST(test_jit_phi_select_loop_carried);
    RUN_TEST(test_jit_internal_global_load_store);
    RUN_TEST(test_jit_internal_global_address_relocation);
    RUN_TEST(test_jit_external_call_abs);
    RUN_TEST(test_jit_varargs_printf_call);
    RUN_TEST(test_jit_varargs_printf_double_call);
    RUN_TEST(test_jit_const_gep_vtable_function_ptr);
    RUN_TEST(test_jit_llvm_intrinsic_fabs_f32);
    RUN_TEST(test_jit_llvm_intrinsic_memcpy_memset);
    RUN_TEST(test_jit_gep_struct_field);
    RUN_TEST(test_jit_gep_array_index);
    RUN_TEST(test_jit_gep_negative_i32_index);
    RUN_TEST(test_jit_global_string_constant);
    RUN_TEST(test_jit_global_struct_ptr_relocation);
    RUN_TEST(test_jit_global_struct_integer_init);
    RUN_TEST(test_jit_aggregate_load_store_copy);
    RUN_TEST(test_jit_call_stack_args);
    RUN_TEST(test_jit_call_many_stack_args);
    RUN_TEST(test_jit_fsub_double);
    RUN_TEST(test_jit_fdiv_double);
    RUN_TEST(test_jit_fneg_double);
    RUN_TEST(test_jit_sitofp_i64_f64);
    RUN_TEST(test_jit_fptosi_f64_i64);
    RUN_TEST(test_jit_fpext_f32_f64);
    RUN_TEST(test_jit_fptrunc_f64_f32);
    RUN_TEST(test_jit_fcmp_oeq);
    RUN_TEST(test_jit_fp_arithmetic_chain);
    RUN_TEST(test_jit_insert_extractvalue_struct_fields);
    RUN_TEST(test_jit_late_frame_patch_and_phi_slots);
    RUN_TEST(test_jit_packed_struct_float_constant);
    RUN_TEST(test_jit_packed_struct_double_constant);

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

    fprintf(stderr, "\nBuilder API tests:\n");
    RUN_TEST(test_builder_ret_42);
    RUN_TEST(test_builder_add_args);
    RUN_TEST(test_builder_arithmetic);
    RUN_TEST(test_builder_icmp_branch);
    RUN_TEST(test_builder_loop_phi);
    RUN_TEST(test_builder_alloca_load_store);
    RUN_TEST(test_builder_gep_runtime_index_canonicalized_i64);
    RUN_TEST(test_builder_call);
    RUN_TEST(test_builder_select);
    RUN_TEST(test_builder_roundtrip);

    fprintf(stderr, "\nObject file tests:\n");
#if !defined(__APPLE__)
    RUN_TEST(test_objfile_elf_header);
    RUN_TEST(test_objfile_elf_symbols);
    RUN_TEST(test_objfile_elf_call_relocation);
    RUN_TEST(test_objfile_elf_readelf_validates);
#else
    RUN_TEST(test_objfile_macho_header);
#endif

    fprintf(stderr, "\n================\n");
    fprintf(stderr, "%d tests: %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
