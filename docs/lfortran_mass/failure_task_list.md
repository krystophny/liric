# LFortran + Liric Failure Task List

Generated: 2026-02-19 10:23:44 UTC

Source artifacts:
- summary: `/tmp/liric_lfortran_mass_branch/summary.json`
- results: `/tmp/liric_lfortran_mass_branch/results.jsonl`

## Snapshot

- total: 2361
- pass: 2278
- mismatch: 55
- lfortran_emit_fail: 26
- unsupported_abi: 2
- gate_fail: true

## mismatch (55)

- [ ] nested_callback_interface_01 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] max_02 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] min_02 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] subroutines_13 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] functions_09 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] functions_10 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] functions_21 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] functions_28 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] array_04_transfer (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] array_08_transfer (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] arrays_35 (`output-format|rc-mismatch|general`) - differential mismatch
- [ ] arrays_38 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] arrays_81 (`output-format|rc-mismatch|general`) - differential mismatch
- [ ] global_allocatable_02 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] pointer_02 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] arrays_constructor_01 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] format_58 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_24 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_29 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_39 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_74 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_125 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_126 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_145 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_149 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_159 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_282 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_298 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_319 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_338 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_352 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_353 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_358 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_389 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] intrinsics_400 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] modules_38 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] bindc3 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] bindc4 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] select_type_05 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] where_08 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] complex_16 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] file_19 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] file_22 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] file_32 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] class_48 (`output-format|rc-mismatch|general`) - differential mismatch
- [ ] sin_02 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] cpu_time_01 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] system_clock_01 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] cmd_01 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] external_11 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] read_02 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] common_03 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] c_ptr_04 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] c_ptr_07 (`output-format|wrong-stdout|general`) - differential mismatch
- [ ] matmul_01 (`output-format|wrong-stdout|general`) - differential mismatch

## lfortran_emit_fail (26)

- [ ] include_03 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] while_03 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] subroutines_14 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_04_size (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] integer_bin_op_dim_external_module (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_11 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_18 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_20 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_22 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_23 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_24 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_26 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_op_29 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] arrays_33 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] select_type_28 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] custom_unary_operator_02 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] allocate_12 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] class_116 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] class_118 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] array_section_01 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] nested_vars_01 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] pass_array_by_data_03 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] pass_array_by_data_07 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] write_16 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] write_18 (`codegen|compiler-error|general`) - lfortran emission/native execution failed
- [ ] legacy_array_sections_09 (`codegen|compiler-error|general`) - lfortran emission/native execution failed

## unsupported_abi (2)

- [ ] dict_test_07_ (`jit-link|unresolved-symbol|runtime-api`) - liric jit ABI/link failure
- [ ] dict_test_13_ (`jit-link|unresolved-symbol|runtime-api`) - liric jit ABI/link failure

