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
int test_parser_rejects_mismatched_vreg_types(void);
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
int test_parser_phi_many_incoming_pairs(void);
int test_parser_select_with_ptr_operands(void);
int test_parser_bitcast_const_expr_operand(void);
int test_parser_function_pointer_type(void);
int test_parser_quoted_label_names(void);
int test_parser_boolean_literals(void);
int test_parser_named_params_no_collision(void);
int test_parser_unnamed_params_numeric_alias(void);
int test_parser_high_numeric_vregs(void);
int test_parser_dynamic_vreg_map_growth(void);
int test_parser_dynamic_block_map_growth(void);
int test_parser_dynamic_global_map_growth(void);
int test_parser_dynamic_func_map_growth(void);
int test_parser_cast_expr_in_aggregate_init(void);
int test_parser_streaming_callback_order(void);
int test_parser_streaming_callback_error_propagates(void);
int test_parser_vector_type_roundtrip(void);
int test_codegen_ret_42(void);
int test_codegen_add(void);
int test_codegen_skip_redundant_immediate_reload(void);
int test_codegen_reuse_cached_vreg_across_scratch_regs(void);
int test_codegen_keep_store_for_next_inst_multiuse_vreg(void);
int test_codegen_zero_immediate_uses_xor_when_flags_dead(void);
int test_codegen_select_zero_keeps_mov_for_flags(void);
int test_codegen_x86_global_reloc_uses_abs64_when_jit_and_objctx(void);
int test_host_target_name(void);
int test_create_host_target(void);
int test_create_unknown_target_fails(void);
int test_non_host_target_fails(void);
int test_load_missing_runtime_library_fails(void);
int test_target_alias_arm64_resolves(void);
int test_target_riscv64_split_resolves(void);
int test_target_copy_patch_entrypoints_available(void);
int test_target_requires_full_streaming_hooks(void);
int test_target_copy_patch_fallback_matches_isel_for_non_x86(void);
int test_target_copy_patch_matches_isel_for_x86_streaming(void);
int test_target_x86_streaming_hooks_isel_smoke(void);
int test_target_x86_streaming_hooks_copy_patch_smoke(void);
int test_target_x86_streaming_hooks_phi_smoke(void);
int test_target_aarch64_streaming_hooks_smoke(void);
int test_target_aarch64_streaming_fp_convert_ops(void);
int test_target_riscv64_streaming_hooks_smoke(void);
int test_target_riscv64_streaming_reports_unsupported_ops(void);
int test_parse_auto_selects_ll_frontend(void);
int test_parse_auto_selects_wasm_frontend(void);
int test_parse_auto_selects_bc_frontend(void);
int test_platform_jit_page_transitions(void);
int test_platform_time_ns_monotonic(void);
int test_platform_dlsym_default_malloc(void);
int test_platform_run_process_exit_status(void);
int test_symbol_provider_prefers_jit_table(void);
int test_target_shared_static_alloca_table(void);
int test_ir_finalize_builds_dense_arrays(void);
int test_ir_finalize_peephole_constant_identity_and_branch(void);
int test_ir_finalize_redundant_load_elimination(void);
int test_ir_finalize_redundant_load_kept_after_store(void);
int test_ir_inst_create_packs_operands_in_single_allocation(void);
int test_ir_phi_copies_flat_arrays_preserve_emission_order(void);
int test_headers_share_opcode_and_operand_types(void);
int test_jit_add_symbol_updates_cached_lookup(void);
int test_jit_ret_42(void);
int test_jit_mode_llvm_contract(void);
int test_jit_lazy_repeated_lookup_returns_ready_symbol(void);
int test_jit_add_args(void);
int test_jit_arithmetic(void);
int test_jit_icmp(void);
int test_jit_select_immediate_zero(void);
int test_jit_branch(void);
int test_jit_loop(void);
int test_jit_alloca_load_store(void);
int test_jit_typeless_load_defaults_to_ptr_width(void);
int test_jit_alloca_many_static_slots(void);
int test_jit_forward_typed_call(void);
int test_jit_forward_call_chain(void);
int test_jit_batched_module_updates(void);
int test_jit_self_recursive_call(void);
int test_jit_self_recursive_call_ignores_prebound_symbol(void);
int test_jit_lazy_function_ignores_prebound_symbol(void);
int test_jit_unresolved_symbol_fails(void);
int test_jit_lazy_materializes_reachable_functions_only(void);
int test_jit_parallel_prefetch_replays_pending_functions(void);
int test_jit_parallel_prefetch_caches_transitive_chain(void);
int test_jit_parallel_prefetch_stress_repeatable(void);
int test_jit_materialization_cache_reuse_across_jits(void);
int test_jit_materialization_cache_invalidation_epoch(void);
int test_jit_fadd_double_bits(void);
int test_jit_fmul_float_bits(void);
int test_jit_phi_select_nested(void);
int test_jit_phi_select_loop_carried(void);
int test_jit_internal_global_load_store(void);
int test_jit_internal_global_address_relocation(void);
int test_jit_patch_relocs_from_skips_prior_entries(void);
int test_jit_external_call_abs(void);
int test_jit_external_call_abs_twice(void);
int test_jit_varargs_printf_call(void);
int test_jit_varargs_printf_double_call(void);
int test_jit_varargs_declared_signature_call(void);
int test_jit_varargs_undeclared_signature_call(void);
int test_jit_const_gep_vtable_function_ptr(void);
int test_jit_llvm_intrinsic_fabs_f32(void);
int test_jit_llvm_intrinsic_extended_blob_coverage(void);
int test_jit_llvm_intrinsic_powi_f32_i32(void);
int test_jit_llvm_intrinsic_memcpy_memset(void);
int test_jit_llvm_intrinsic_memmove(void);
int test_jit_gep_struct_field(void);
int test_jit_gep_array_index(void);
int test_jit_gep_negative_i32_index(void);
int test_jit_cast_i1_sign_and_zero_extend(void);
int test_jit_trunc_i64_to_i16_then_zext(void);
int test_jit_sext_i8_to_i64(void);
int test_jit_global_string_constant(void);
int test_jit_global_struct_ptr_relocation(void);
int test_jit_global_struct_integer_init(void);
int test_jit_global_struct_inttoptr_immediate_init(void);
int test_jit_aggregate_load_store_copy(void);
int test_jit_call_stack_args(void);
int test_jit_call_many_stack_args(void);
int test_jit_call_gt16_args(void);
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
int test_wasm_ir_i64_unsigned_div_rem_lower_to_integer_ops(void);
int test_wasm_to_session_builds_function_ir(void);
int test_wasm_to_session_invalid_arguments(void);
int test_wasm_jit_ret_42(void);
int test_wasm_jit_add_args(void);
int test_wasm_jit_div_u_opcodes_lower(void);
int test_wasm_jit_rem_u_opcodes_lower(void);
int test_wasm_jit_branch(void);
int test_wasm_jit_loop(void);
int test_wasm_jit_call(void);
int test_bc_parse_api_ret_42(void);
int test_bc_parse_auto_loop_phi(void);
int test_bc_module_constants_update_global_value_table(void);
int test_bc_streaming_callback_collects_opcodes(void);
int test_bc_streaming_callback_abort_propagates_error(void);
int test_bc_streaming_callback_dump_text_shape(void);
int test_session_direct_ret_42(void);
int test_session_add_args(void);
int test_session_arithmetic_chain(void);
int test_session_stream_stencil_fast_path(void);
int test_session_stream_isel_fast_path(void);
int test_session_direct_llvm_mode_stream_contract(void);
int test_session_direct_llvm_forward_ref_lookup_contract(void);
int test_session_direct_forward_ref_lookup_contract(void);
int test_session_direct_forward_global_lookup_contract(void);
int test_session_explicit_backend_overrides_env(void);
int test_session_stream_stencil_no_ir_fallback(void);
int test_session_add_phi_copy_api(void);
int test_session_icmp_branch(void);
int test_session_alloca_load_store(void);
int test_session_loop_phi(void);
int test_session_call(void);
int test_session_operand_global_offset_propagates_to_ir(void);
int test_session_select(void);
int test_session_ir_print(void);
int test_session_ir_lookup_prefers_module_symbol_over_process_symbol(void);
int test_session_ll_compile(void);
int test_session_bc_compile(void);
int test_session_auto_compile_ll_and_bc(void);
int test_session_multiple_functions(void);
int test_session_emit_object_llvm_mode_contract(void);
int test_merge_two_independent_functions(void);
int test_merge_declaration_replaced_by_definition(void);
int test_merge_global_definition(void);
int test_merge_jit_runs_merged_function(void);
int test_builder_compat_add_to_jit(void);
int test_builder_compat_direct_sparse_block_ids_finalize(void);
int test_builder_compat_direct_multi_suspend_reloc_ranges(void);
int test_builder_compat_add_to_jit_null_args(void);
int test_builder_compat_memory_and_call_path(void);
int test_builder_compat_phi_finalize_add_incoming_after_finalize_noop(void);
int test_builder_compat_direct_llvm_phi_incoming_sync(void);
int test_builder_compat_emit_object_to_file(void);
int test_builder_compat_emit_object_llvm_mode_contract(void);
int test_builder_compat_jit_exec(void);
int test_builder_compat_jit_exec_with_call(void);
int test_builder_compat_load_library_null_rejects(void);
int test_builder_compat_emit_executable_llvm_mode_contract(void);
int test_builder_compat_direct_large_object_emission(void);
int test_llvm_c_shim_add_and_lookup(void);
int test_llvm_c_shim_lookup_float_return_uses_host_abi(void);
int test_llvm_c_shim_rejects_undeclared_data_global(void);
int test_llvm_c_shim_load_library_rejects_null(void);
#if !defined(__APPLE__)
int test_objfile_elf_header(void);
int test_objfile_elf_symbols(void);
int test_objfile_elf_lfortran_module_init_symbol_is_weak(void);
int test_objfile_elf_call_relocation(void);
int test_objfile_elf_readelf_validates(void);
int test_objfile_elf_executable_aarch64_header(void);
int test_objfile_session_emit_object_stream_direct(void);
int test_objfile_session_emit_object_stream_ir(void);
#if defined(__linux__)
int test_objfile_elf_exe_runs(void);
int test_objfile_link_and_run(void);
int test_objfile_link_and_run_direct(void);
int test_dynelf_puts_hello(void);
int test_dynelf_readelf_dynamic(void);
int test_dynelf_ldd_check(void);
#endif
#else
int test_objfile_macho_header(void);
#endif

#if defined(__linux__)
int test_session_ir_exe_ret_42(void);
int test_session_ir_exe_branch(void);
int test_session_ir_exe_call(void);
int test_session_ir_exe_loop(void);
int test_session_direct_exe_ret_42(void);
int test_session_direct_exe_branch(void);
int test_session_direct_exe_call(void);
int test_session_direct_jit_and_exe(void);
#endif

#if defined(__x86_64__) || defined(_M_X64)
int test_cp_add_i32(void);
int test_cp_arithmetic_chain_i32(void);
int test_cp_all_alu_ops_i64(void);
int test_cp_shift_ops(void);
int test_cp_sdiv_srem(void);
int test_cp_fallback_to_isel(void);
int test_cp_immediate_operand(void);
int test_cp_add_ret_supernode_i32(void);
int test_cp_add_ret_supernode_i64(void);
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
int test_stencil_generated_lookup_core_entries(void);
int test_stencil_generated_lookup_unknown_returns_null(void);
int test_stencil_gen_deterministic_output(void);
int test_stencil_gen_missing_input_fails(void);
int test_stencil_runtime_lookup_known_entries(void);
int test_stencil_runtime_lookup_unknown_entry_returns_null(void);
int test_stencil_runtime_emit_patches_all_holes(void);
int test_stencil_runtime_emit_strip_trailing_ret(void);
int test_stencil_runtime_emit_rejects_small_buffer(void);
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
    RUN_TEST(test_parser_rejects_mismatched_vreg_types);
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
    RUN_TEST(test_parser_phi_many_incoming_pairs);
    RUN_TEST(test_parser_select_with_ptr_operands);
    RUN_TEST(test_parser_bitcast_const_expr_operand);
    RUN_TEST(test_parser_function_pointer_type);
    RUN_TEST(test_parser_quoted_label_names);
    RUN_TEST(test_parser_boolean_literals);
    RUN_TEST(test_parser_named_params_no_collision);
    RUN_TEST(test_parser_unnamed_params_numeric_alias);
    RUN_TEST(test_parser_high_numeric_vregs);
    RUN_TEST(test_parser_dynamic_vreg_map_growth);
    RUN_TEST(test_parser_dynamic_block_map_growth);
    RUN_TEST(test_parser_dynamic_global_map_growth);
    RUN_TEST(test_parser_dynamic_func_map_growth);
    RUN_TEST(test_parser_cast_expr_in_aggregate_init);
    RUN_TEST(test_parser_streaming_callback_order);
    RUN_TEST(test_parser_streaming_callback_error_propagates);
    RUN_TEST(test_parser_vector_type_roundtrip);

    fprintf(stderr, "\nCodegen tests:\n");
    RUN_TEST(test_codegen_ret_42);
    RUN_TEST(test_codegen_add);
    RUN_TEST(test_codegen_skip_redundant_immediate_reload);
    RUN_TEST(test_codegen_reuse_cached_vreg_across_scratch_regs);
    RUN_TEST(test_codegen_keep_store_for_next_inst_multiuse_vreg);
    RUN_TEST(test_codegen_zero_immediate_uses_xor_when_flags_dead);
    RUN_TEST(test_codegen_select_zero_keeps_mov_for_flags);
    RUN_TEST(test_codegen_x86_global_reloc_uses_abs64_when_jit_and_objctx);

    fprintf(stderr, "\nTarget tests:\n");
    RUN_TEST(test_host_target_name);
    RUN_TEST(test_create_host_target);
    RUN_TEST(test_create_unknown_target_fails);
    RUN_TEST(test_non_host_target_fails);
    RUN_TEST(test_load_missing_runtime_library_fails);
    RUN_TEST(test_target_alias_arm64_resolves);
    RUN_TEST(test_target_riscv64_split_resolves);
    RUN_TEST(test_target_copy_patch_entrypoints_available);
    RUN_TEST(test_target_requires_full_streaming_hooks);
    RUN_TEST(test_target_copy_patch_fallback_matches_isel_for_non_x86);
    RUN_TEST(test_target_copy_patch_matches_isel_for_x86_streaming);
    RUN_TEST(test_target_x86_streaming_hooks_isel_smoke);
    RUN_TEST(test_target_x86_streaming_hooks_copy_patch_smoke);
    RUN_TEST(test_target_x86_streaming_hooks_phi_smoke);
    RUN_TEST(test_target_aarch64_streaming_hooks_smoke);
    RUN_TEST(test_target_aarch64_streaming_fp_convert_ops);
    RUN_TEST(test_target_riscv64_streaming_hooks_smoke);
    RUN_TEST(test_target_riscv64_streaming_reports_unsupported_ops);
    RUN_TEST(test_parse_auto_selects_ll_frontend);
    RUN_TEST(test_parse_auto_selects_wasm_frontend);
    RUN_TEST(test_parse_auto_selects_bc_frontend);
    RUN_TEST(test_platform_jit_page_transitions);
    RUN_TEST(test_platform_time_ns_monotonic);
    RUN_TEST(test_platform_dlsym_default_malloc);
    RUN_TEST(test_platform_run_process_exit_status);
    RUN_TEST(test_symbol_provider_prefers_jit_table);
    RUN_TEST(test_target_shared_static_alloca_table);
    RUN_TEST(test_ir_finalize_builds_dense_arrays);
    RUN_TEST(test_ir_finalize_peephole_constant_identity_and_branch);
    RUN_TEST(test_ir_finalize_redundant_load_elimination);
    RUN_TEST(test_ir_finalize_redundant_load_kept_after_store);
    RUN_TEST(test_ir_inst_create_packs_operands_in_single_allocation);
    RUN_TEST(test_ir_phi_copies_flat_arrays_preserve_emission_order);
    RUN_TEST(test_headers_share_opcode_and_operand_types);

    fprintf(stderr, "\nJIT tests:\n");
    RUN_TEST(test_jit_add_symbol_updates_cached_lookup);
    RUN_TEST(test_jit_ret_42);
    RUN_TEST(test_jit_mode_llvm_contract);
    RUN_TEST(test_jit_lazy_repeated_lookup_returns_ready_symbol);
    RUN_TEST(test_jit_add_args);
    RUN_TEST(test_jit_arithmetic);
    RUN_TEST(test_jit_icmp);
    RUN_TEST(test_jit_select_immediate_zero);
    RUN_TEST(test_jit_branch);
    RUN_TEST(test_jit_loop);
    RUN_TEST(test_jit_alloca_load_store);
    RUN_TEST(test_jit_typeless_load_defaults_to_ptr_width);
    RUN_TEST(test_jit_alloca_many_static_slots);
    RUN_TEST(test_jit_forward_typed_call);
    RUN_TEST(test_jit_forward_call_chain);
    RUN_TEST(test_jit_batched_module_updates);
    RUN_TEST(test_jit_self_recursive_call);
    RUN_TEST(test_jit_self_recursive_call_ignores_prebound_symbol);
    RUN_TEST(test_jit_lazy_function_ignores_prebound_symbol);
    RUN_TEST(test_jit_unresolved_symbol_fails);
    RUN_TEST(test_jit_lazy_materializes_reachable_functions_only);
    RUN_TEST(test_jit_parallel_prefetch_replays_pending_functions);
    RUN_TEST(test_jit_parallel_prefetch_caches_transitive_chain);
    RUN_TEST(test_jit_parallel_prefetch_stress_repeatable);
    RUN_TEST(test_jit_materialization_cache_reuse_across_jits);
    RUN_TEST(test_jit_materialization_cache_invalidation_epoch);
    RUN_TEST(test_jit_fadd_double_bits);
    RUN_TEST(test_jit_fmul_float_bits);
    RUN_TEST(test_jit_phi_select_nested);
    RUN_TEST(test_jit_phi_select_loop_carried);
    RUN_TEST(test_jit_internal_global_load_store);
    RUN_TEST(test_jit_internal_global_address_relocation);
    RUN_TEST(test_jit_patch_relocs_from_skips_prior_entries);
    RUN_TEST(test_jit_external_call_abs);
    RUN_TEST(test_jit_external_call_abs_twice);
    RUN_TEST(test_jit_varargs_printf_call);
    RUN_TEST(test_jit_varargs_printf_double_call);
    RUN_TEST(test_jit_varargs_declared_signature_call);
    RUN_TEST(test_jit_varargs_undeclared_signature_call);
    RUN_TEST(test_jit_const_gep_vtable_function_ptr);
    RUN_TEST(test_jit_llvm_intrinsic_fabs_f32);
    RUN_TEST(test_jit_llvm_intrinsic_extended_blob_coverage);
    RUN_TEST(test_jit_llvm_intrinsic_powi_f32_i32);
    RUN_TEST(test_jit_llvm_intrinsic_memcpy_memset);
    RUN_TEST(test_jit_llvm_intrinsic_memmove);
    RUN_TEST(test_jit_gep_struct_field);
    RUN_TEST(test_jit_gep_array_index);
    RUN_TEST(test_jit_gep_negative_i32_index);
    RUN_TEST(test_jit_cast_i1_sign_and_zero_extend);
    RUN_TEST(test_jit_trunc_i64_to_i16_then_zext);
    RUN_TEST(test_jit_sext_i8_to_i64);
    RUN_TEST(test_jit_global_string_constant);
    RUN_TEST(test_jit_global_struct_ptr_relocation);
    RUN_TEST(test_jit_global_struct_integer_init);
    RUN_TEST(test_jit_global_struct_inttoptr_immediate_init);
    RUN_TEST(test_jit_aggregate_load_store_copy);
    RUN_TEST(test_jit_call_stack_args);
    RUN_TEST(test_jit_call_many_stack_args);
    RUN_TEST(test_jit_call_gt16_args);
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
    RUN_TEST(test_wasm_ir_i64_unsigned_div_rem_lower_to_integer_ops);
    RUN_TEST(test_wasm_to_session_builds_function_ir);
    RUN_TEST(test_wasm_to_session_invalid_arguments);

    fprintf(stderr, "\nWASM JIT tests:\n");
    RUN_TEST(test_wasm_jit_ret_42);
    RUN_TEST(test_wasm_jit_add_args);
    RUN_TEST(test_wasm_jit_div_u_opcodes_lower);
    RUN_TEST(test_wasm_jit_rem_u_opcodes_lower);
    RUN_TEST(test_wasm_jit_branch);
    RUN_TEST(test_wasm_jit_loop);
    RUN_TEST(test_wasm_jit_call);

    fprintf(stderr, "\nBC Parser tests:\n");
    RUN_TEST(test_bc_parse_api_ret_42);
    RUN_TEST(test_bc_parse_auto_loop_phi);
    RUN_TEST(test_bc_module_constants_update_global_value_table);
    RUN_TEST(test_bc_streaming_callback_collects_opcodes);
    RUN_TEST(test_bc_streaming_callback_abort_propagates_error);
    RUN_TEST(test_bc_streaming_callback_dump_text_shape);

    fprintf(stderr, "\nSession API tests:\n");
    RUN_TEST(test_session_direct_ret_42);
    RUN_TEST(test_session_add_args);
    RUN_TEST(test_session_arithmetic_chain);
    RUN_TEST(test_session_stream_stencil_fast_path);
    RUN_TEST(test_session_stream_isel_fast_path);
    RUN_TEST(test_session_direct_llvm_mode_stream_contract);
    RUN_TEST(test_session_direct_llvm_forward_ref_lookup_contract);
    RUN_TEST(test_session_direct_forward_ref_lookup_contract);
    RUN_TEST(test_session_direct_forward_global_lookup_contract);
    RUN_TEST(test_session_explicit_backend_overrides_env);
    RUN_TEST(test_session_stream_stencil_no_ir_fallback);
    RUN_TEST(test_session_add_phi_copy_api);
    RUN_TEST(test_session_icmp_branch);
    RUN_TEST(test_session_alloca_load_store);
    RUN_TEST(test_session_loop_phi);
    RUN_TEST(test_session_call);
    RUN_TEST(test_session_operand_global_offset_propagates_to_ir);
    RUN_TEST(test_session_select);
    RUN_TEST(test_session_ir_print);
    RUN_TEST(test_session_ir_lookup_prefers_module_symbol_over_process_symbol);
    RUN_TEST(test_session_ll_compile);
    RUN_TEST(test_session_bc_compile);
    RUN_TEST(test_session_auto_compile_ll_and_bc);
    RUN_TEST(test_session_multiple_functions);
    RUN_TEST(test_session_emit_object_llvm_mode_contract);

#if defined(__linux__)
    fprintf(stderr, "\nSession IR exe tests:\n");
    RUN_TEST(test_session_ir_exe_ret_42);
    RUN_TEST(test_session_ir_exe_branch);
    RUN_TEST(test_session_ir_exe_call);
    RUN_TEST(test_session_ir_exe_loop);

    fprintf(stderr, "\nSession DIRECT exe tests:\n");
    RUN_TEST(test_session_direct_exe_ret_42);
    RUN_TEST(test_session_direct_exe_branch);
    RUN_TEST(test_session_direct_exe_call);
    RUN_TEST(test_session_direct_jit_and_exe);
#endif

    fprintf(stderr, "\nModule merge tests:\n");
    RUN_TEST(test_merge_two_independent_functions);
    RUN_TEST(test_merge_declaration_replaced_by_definition);
    RUN_TEST(test_merge_global_definition);
    RUN_TEST(test_merge_jit_runs_merged_function);

    fprintf(stderr, "\nCompat API tests:\n");
    RUN_TEST(test_builder_compat_add_to_jit);
    RUN_TEST(test_builder_compat_direct_sparse_block_ids_finalize);
    RUN_TEST(test_builder_compat_direct_multi_suspend_reloc_ranges);
    RUN_TEST(test_builder_compat_add_to_jit_null_args);
    RUN_TEST(test_builder_compat_memory_and_call_path);
    RUN_TEST(test_builder_compat_phi_finalize_add_incoming_after_finalize_noop);
    RUN_TEST(test_builder_compat_direct_llvm_phi_incoming_sync);
    RUN_TEST(test_builder_compat_emit_object_to_file);
    RUN_TEST(test_builder_compat_emit_object_llvm_mode_contract);
    RUN_TEST(test_builder_compat_jit_exec);
    RUN_TEST(test_builder_compat_jit_exec_with_call);
    RUN_TEST(test_builder_compat_load_library_null_rejects);
    RUN_TEST(test_builder_compat_emit_executable_llvm_mode_contract);
    RUN_TEST(test_builder_compat_direct_large_object_emission);
    RUN_TEST(test_llvm_c_shim_add_and_lookup);
    RUN_TEST(test_llvm_c_shim_lookup_float_return_uses_host_abi);
    RUN_TEST(test_llvm_c_shim_rejects_undeclared_data_global);
    RUN_TEST(test_llvm_c_shim_load_library_rejects_null);

    fprintf(stderr, "\nObject file tests:\n");
#if !defined(__APPLE__)
    RUN_TEST(test_objfile_elf_header);
    RUN_TEST(test_objfile_elf_symbols);
    RUN_TEST(test_objfile_elf_lfortran_module_init_symbol_is_weak);
    RUN_TEST(test_objfile_elf_call_relocation);
    RUN_TEST(test_objfile_elf_readelf_validates);
    RUN_TEST(test_objfile_elf_executable_aarch64_header);
    RUN_TEST(test_objfile_session_emit_object_stream_direct);
    RUN_TEST(test_objfile_session_emit_object_stream_ir);
#if defined(__linux__)
    RUN_TEST(test_objfile_elf_exe_runs);
    RUN_TEST(test_objfile_link_and_run);
    RUN_TEST(test_objfile_link_and_run_direct);
    RUN_TEST(test_dynelf_puts_hello);
    RUN_TEST(test_dynelf_readelf_dynamic);
    RUN_TEST(test_dynelf_ldd_check);
#endif
#else
    RUN_TEST(test_objfile_macho_header);
#endif

#if defined(__x86_64__) || defined(_M_X64)
    fprintf(stderr, "\nCopy-and-patch tests:\n");
    RUN_TEST(test_cp_add_i32);
    RUN_TEST(test_cp_arithmetic_chain_i32);
    RUN_TEST(test_cp_all_alu_ops_i64);
    RUN_TEST(test_cp_shift_ops);
    RUN_TEST(test_cp_sdiv_srem);
    RUN_TEST(test_cp_fallback_to_isel);
    RUN_TEST(test_cp_immediate_operand);
    RUN_TEST(test_cp_add_ret_supernode_i32);
    RUN_TEST(test_cp_add_ret_supernode_i64);
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    fprintf(stderr, "\nStencil generator tests:\n");
    RUN_TEST(test_stencil_generated_lookup_core_entries);
    RUN_TEST(test_stencil_generated_lookup_unknown_returns_null);
    RUN_TEST(test_stencil_gen_deterministic_output);
    RUN_TEST(test_stencil_gen_missing_input_fails);

    fprintf(stderr, "\nStencil runtime tests:\n");
    RUN_TEST(test_stencil_runtime_lookup_known_entries);
    RUN_TEST(test_stencil_runtime_lookup_unknown_entry_returns_null);
    RUN_TEST(test_stencil_runtime_emit_patches_all_holes);
    RUN_TEST(test_stencil_runtime_emit_strip_trailing_ret);
    RUN_TEST(test_stencil_runtime_emit_rejects_small_buffer);
#endif

    fprintf(stderr, "\n================\n");
    fprintf(stderr, "%d tests: %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
