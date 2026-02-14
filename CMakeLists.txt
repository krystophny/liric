cmake_minimum_required(VERSION 3.10)
project(liric VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_FLAGS_RELEASE "-O2 -g -fno-omit-frame-pointer -DNDEBUG"
    CACHE STRING "Release C flags" FORCE)
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer -DNDEBUG"
    CACHE STRING "RelWithDebInfo C flags" FORCE)

option(LIRIC_ENABLE_LTO "Enable interprocedural optimization (LTO/IPO)" ON)
if(LIRIC_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LIRIC_IPO_SUPPORTED OUTPUT LIRIC_IPO_ERROR)
    if(LIRIC_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    else()
        message(WARNING "LTO requested but unsupported: ${LIRIC_IPO_ERROR}")
    endif()
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

option(WITH_REAL_LLVM_BACKEND "Enable real LLVM C API backend for mode 'llvm'" OFF)
set(LIRIC_LLVM_CONFIG_EXE "" CACHE FILEPATH "Path to llvm-config for real LLVM backend")
set(LIRIC_LLVM_COMPAT_MIN_MAJOR "11")

set(LIRIC_REAL_LLVM_INCLUDE_DIR "")
set(LIRIC_REAL_LLVM_CFLAGS_LIST "")
set(LIRIC_REAL_LLVM_LDFLAGS_LIST "")
set(LIRIC_REAL_LLVM_LIBS_LIST "")
set(LIRIC_REAL_LLVM_SYSTEM_LIBS_LIST "")
set(LIRIC_BACKEND_LLVM_VERSION "")
set(LIRIC_BACKEND_LLVM_VERSION_MAJOR "0")

if(WITH_REAL_LLVM_BACKEND)
    if(LIRIC_LLVM_CONFIG_EXE)
        set(LLVM_BACKEND_CONFIG_EXE "${LIRIC_LLVM_CONFIG_EXE}")
    else()
        find_program(LLVM_BACKEND_CONFIG_EXE llvm-config)
    endif()
    if(NOT LLVM_BACKEND_CONFIG_EXE)
        message(FATAL_ERROR "WITH_REAL_LLVM_BACKEND=ON requires llvm-config (or set LIRIC_LLVM_CONFIG_EXE)")
    endif()

    execute_process(
        COMMAND ${LLVM_BACKEND_CONFIG_EXE} --version
        OUTPUT_VARIABLE LIRIC_BACKEND_LLVM_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCH "^[0-9]+" LIRIC_BACKEND_LLVM_VERSION_MAJOR "${LIRIC_BACKEND_LLVM_VERSION}")
    if(NOT LIRIC_BACKEND_LLVM_VERSION_MAJOR)
        message(FATAL_ERROR "Could not parse LLVM major version from: ${LIRIC_BACKEND_LLVM_VERSION}")
    endif()
    string(REGEX MATCH "^[0-9]+\\.([0-9]+)" _ver_match "${LIRIC_BACKEND_LLVM_VERSION}")
    set(LIRIC_BACKEND_LLVM_VERSION_MINOR "${CMAKE_MATCH_1}")
    if(NOT LIRIC_BACKEND_LLVM_VERSION_MINOR)
        set(LIRIC_BACKEND_LLVM_VERSION_MINOR 0)
    endif()
    math(EXPR LIRIC_BACKEND_LLVM_VERSION_NUM
        "${LIRIC_BACKEND_LLVM_VERSION_MAJOR} * 100 + ${LIRIC_BACKEND_LLVM_VERSION_MINOR}")

    execute_process(
        COMMAND ${LLVM_BACKEND_CONFIG_EXE} --includedir
        OUTPUT_VARIABLE LIRIC_REAL_LLVM_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_BACKEND_CONFIG_EXE} --cflags
        OUTPUT_VARIABLE LIRIC_REAL_LLVM_CFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_BACKEND_CONFIG_EXE} --ldflags
        OUTPUT_VARIABLE LIRIC_REAL_LLVM_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_BACKEND_CONFIG_EXE} --libs all
        OUTPUT_VARIABLE LIRIC_REAL_LLVM_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_BACKEND_CONFIG_EXE} --system-libs
        OUTPUT_VARIABLE LIRIC_REAL_LLVM_SYSTEM_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    separate_arguments(LIRIC_REAL_LLVM_CFLAGS_LIST NATIVE_COMMAND "${LIRIC_REAL_LLVM_CFLAGS}")
    separate_arguments(LIRIC_REAL_LLVM_LDFLAGS_LIST NATIVE_COMMAND "${LIRIC_REAL_LLVM_LDFLAGS}")
    separate_arguments(LIRIC_REAL_LLVM_LIBS_LIST NATIVE_COMMAND "${LIRIC_REAL_LLVM_LIBS}")
    separate_arguments(LIRIC_REAL_LLVM_SYSTEM_LIBS_LIST NATIVE_COMMAND "${LIRIC_REAL_LLVM_SYSTEM_LIBS}")

    add_compile_definitions(
        LIRIC_HAVE_REAL_LLVM_BACKEND=1
        LIRIC_BACKEND_LLVM_VERSION_MAJOR=${LIRIC_BACKEND_LLVM_VERSION_MAJOR}
        LIRIC_BACKEND_LLVM_VERSION_NUM=${LIRIC_BACKEND_LLVM_VERSION_NUM}
    )
    message(STATUS "Real LLVM backend enabled (llvm-config=${LLVM_BACKEND_CONFIG_EXE}, version=${LIRIC_BACKEND_LLVM_VERSION})")
endif()

option(WITH_LLVM_COMPAT "Build LLVM C++ compatibility layer tests" OFF)
if(WITH_REAL_LLVM_BACKEND AND WITH_LLVM_COMPAT)
    if(LIRIC_BACKEND_LLVM_VERSION_MAJOR LESS LIRIC_LLVM_COMPAT_MIN_MAJOR)
        message(FATAL_ERROR
            "Unsupported configuration: WITH_REAL_LLVM_BACKEND=ON and WITH_LLVM_COMPAT=ON "
            "requires LLVM >= ${LIRIC_LLVM_COMPAT_MIN_MAJOR}, but detected LLVM "
            "${LIRIC_BACKEND_LLVM_VERSION}. Reconfigure with -DWITH_LLVM_COMPAT=OFF for this LLVM version."
        )
    endif()
endif()

if(UNIX AND NOT APPLE)
    add_link_options("$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-rdynamic>")
endif()

add_library(liric STATIC
    src/arena.c
    src/ir.c
    src/ll_lexer.c
    src/ll_parser.c
    src/frontend_common.c
    src/compile_mode.c
    src/bc_decode.c
    src/frontend_registry.c
    src/wasm_decode.c
    src/wasm_to_ir.c
    src/target_registry.c
    src/target_common.c
    src/target_shared.c
    src/target_x86_64.c
    src/target_aarch64.c
    src/target_riscv64.c
    src/jit.c
    src/liric.c
    src/liric_compat.c
    src/stencil_data.c
    src/stencil_runtime.c
    src/session.c
    src/module_emit.c
    src/llvm_backend.c
    src/objfile.c
    src/objfile_macho.c
    src/objfile_elf.c
    src/llvm_stubs.c
    src/platform/platform_intrinsics.c
    src/platform/platform_os.c
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    target_sources(liric PRIVATE
        src/platform/platform_intrinsic_stubs_x86_64_linux.S
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_sources(liric PRIVATE src/platform/platform_intrinsic_stubs_aarch64_linux.S)
endif()
target_include_directories(liric PUBLIC include)
target_include_directories(liric PRIVATE src)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_executable(stencil_gen tools/stencil_gen.c)
    target_include_directories(stencil_gen PRIVATE src)

    set(LIRIC_STENCIL_INPUTS
        ${CMAKE_CURRENT_SOURCE_DIR}/stencils/add_i32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/stencils/sub_i64.c
        ${CMAKE_CURRENT_SOURCE_DIR}/stencils/fadd_f64.c
    )
    set(LIRIC_STENCIL_DATA_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/generated/stencil_data_x86_64.h
    )
    add_custom_command(
        OUTPUT ${LIRIC_STENCIL_DATA_HEADER}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND $<TARGET_FILE:stencil_gen>
            --compiler ${CMAKE_C_COMPILER}
            --input-dir ${CMAKE_CURRENT_SOURCE_DIR}/stencils
            --output ${LIRIC_STENCIL_DATA_HEADER}
        DEPENDS stencil_gen ${LIRIC_STENCIL_INPUTS}
        VERBATIM
    )
    add_custom_target(liric_stencil_data DEPENDS ${LIRIC_STENCIL_DATA_HEADER})
    add_dependencies(liric liric_stencil_data)
    target_include_directories(liric PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(liric PRIVATE LIRIC_HAVE_GENERATED_STENCILS=1)
endif()
if(WITH_REAL_LLVM_BACKEND)
    target_include_directories(liric PRIVATE "${LIRIC_REAL_LLVM_INCLUDE_DIR}")
    target_compile_options(liric PRIVATE ${LIRIC_REAL_LLVM_CFLAGS_LIST})
    target_compile_definitions(liric PRIVATE
        LIRIC_HAVE_REAL_LLVM_BACKEND=1
        LIRIC_BACKEND_LLVM_VERSION_MAJOR=${LIRIC_BACKEND_LLVM_VERSION_MAJOR}
    )
    target_link_options(liric PUBLIC ${LIRIC_REAL_LLVM_LDFLAGS_LIST})
    target_link_libraries(liric PRIVATE
        ${LIRIC_REAL_LLVM_LIBS_LIST}
        ${LIRIC_REAL_LLVM_SYSTEM_LIBS_LIST}
        stdc++
    )
endif()

add_executable(liric_bin tools/liric_main.c)
set_target_properties(liric_bin PROPERTIES OUTPUT_NAME liric)
set(LIRIC_PLATFORM_LIBS)
find_package(Threads REQUIRED)
if(UNIX AND NOT APPLE)
    list(APPEND LIRIC_PLATFORM_LIBS dl)
endif()
if(UNIX)
    list(APPEND LIRIC_PLATFORM_LIBS m)
endif()
list(APPEND LIRIC_PLATFORM_LIBS Threads::Threads)
target_link_libraries(liric_bin PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(liric_bin PRIVATE src)

add_executable(liric_probe_runner tools/liric_probe_runner.c)
target_link_libraries(liric_probe_runner PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(liric_probe_runner PRIVATE src)

add_executable(bench_compat_check tools/bench_compat_check.c)
target_link_libraries(bench_compat_check PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_ll tools/bench_ll.c)
target_link_libraries(bench_ll PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_api tools/bench_api.c)
target_link_libraries(bench_api PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_api_callgrind_hot tools/bench_api_callgrind_hot.c)
target_link_libraries(bench_api_callgrind_hot PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_corpus tools/bench_corpus.c)
target_link_libraries(bench_corpus PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_exe_matrix tools/bench_exe_matrix.c)
target_link_libraries(bench_exe_matrix PRIVATE ${LIRIC_PLATFORM_LIBS})

option(WITH_BENCH_TCC "Build bench_tcc target (requires libtcc)" ON)
if(WITH_BENCH_TCC)
    find_path(LIRTCC_INCLUDE_DIR libtcc.h)
    find_library(LIRTCC_LIB tcc)
    if(LIRTCC_INCLUDE_DIR AND LIRTCC_LIB)
        add_executable(bench_tcc tools/bench_tcc.c)
        target_include_directories(bench_tcc PRIVATE src "${LIRTCC_INCLUDE_DIR}")
        target_link_libraries(bench_tcc PRIVATE liric "${LIRTCC_LIB}" ${LIRIC_PLATFORM_LIBS})
    else()
        message(STATUS "libtcc not found: bench_tcc target will not be built")
    endif()
else()
    message(STATUS "bench_tcc target disabled (WITH_BENCH_TCC=OFF)")
endif()

option(WITH_BENCH_LLI_PHASES "Build bench_lli_phases when LLVM C ORC APIs are available" ON)
find_program(LLVM_CONFIG_EXE llvm-config)
if(LLVM_CONFIG_EXE AND WITH_BENCH_LLI_PHASES)
    execute_process(
        COMMAND ${LLVM_CONFIG_EXE} --includedir
        OUTPUT_VARIABLE LLVM_INCLUDE_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_CONFIG_EXE} --ldflags
        OUTPUT_VARIABLE LLVM_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_CONFIG_EXE} --libs core orcjit native irreader
        OUTPUT_VARIABLE LLVM_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_CONFIG_EXE} --system-libs
        OUTPUT_VARIABLE LLVM_SYSTEM_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    separate_arguments(LLVM_LDFLAGS_LIST NATIVE_COMMAND "${LLVM_LDFLAGS}")
    separate_arguments(LLVM_LIBS_LIST NATIVE_COMMAND "${LLVM_LIBS}")
    separate_arguments(LLVM_SYSTEM_LIBS_LIST NATIVE_COMMAND "${LLVM_SYSTEM_LIBS}")

    if(EXISTS "${LLVM_INCLUDE_DIR}/llvm-c/LLJIT.h")
        add_executable(bench_lli_phases tools/bench_lli_phases.c)
        target_include_directories(bench_lli_phases PRIVATE ${LLVM_INCLUDE_DIR})
        target_link_options(bench_lli_phases PRIVATE ${LLVM_LDFLAGS_LIST})
        target_link_libraries(
            bench_lli_phases PRIVATE
            ${LLVM_LIBS_LIST}
            ${LLVM_SYSTEM_LIBS_LIST}
            ${LIRIC_PLATFORM_LIBS}
            stdc++
        )
    else()
        message(STATUS "llvm-c/LLJIT.h not found: bench_lli_phases target will not be built")
    endif()
else()
    if(WITH_BENCH_LLI_PHASES)
        message(STATUS "llvm-config not found: bench_lli_phases target will not be built")
    else()
        message(STATUS "bench_lli_phases target disabled (WITH_BENCH_LLI_PHASES=OFF)")
    endif()
endif()

enable_testing()
add_executable(test_liric
    tests/test_main.c
    tests/test_lexer.c
    tests/test_parser.c
    tests/test_codegen.c
    tests/test_targets.c
    tests/test_target_shared.c
    tests/test_header_shared.c
    tests/test_jit.c
    tests/test_e2e.c
    tests/test_bc.c
    tests/test_wasm.c
    tests/test_builder.c
    tests/test_session.c
    tests/test_merge.c
    tests/test_objfile.c
    tests/test_cp.c
    tests/test_stencil_gen.c
    tests/test_stencil_runtime.c
    tests/test_platform_os.c
)
target_link_libraries(test_liric PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(test_liric PRIVATE src)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    target_compile_definitions(test_liric PRIVATE
        LIRIC_STENCIL_GEN_EXE="$<TARGET_FILE:stencil_gen>"
        LIRIC_STENCIL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/stencils"
        LIRIC_STENCIL_CC="${CMAKE_C_COMPILER}"
    )
endif()
add_test(NAME liric_tests COMMAND test_liric)

add_test(
    NAME liric_cli_output_flag_auto_obj_no_main
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/no_main.ll
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/liric_cli_emit_obj_test.o
        -DMODE=auto_object
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

add_test(
    NAME liric_cli_output_flag_o_suffix_forces_obj
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/liric_cli_emit_obj_forced.o
        -DMODE=force_object_suffix
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

add_test(
    NAME liric_cli_default_emits_exe
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_default_emits_exe
        -DMODE=default
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_mode.cmake
)

add_test(
    NAME liric_cli_output_flag_emits_exe
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_output_flag_emits_exe
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_output_flag_emits_exe/liric_cli_custom_out
        -DMODE=output
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_mode.cmake
)

add_test(
    NAME liric_cli_output_flag_jit_conflict
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/liric_cli_emit_obj_conflict.o
        -DMODE=conflict
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

add_test(
    NAME liric_cli_legacy_emit_obj_rejected
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DOUT=${CMAKE_CURRENT_BINARY_DIR}/liric_cli_emit_obj_legacy.o
        -DMODE=legacy_flag_rejected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|aarch64|arm64")
    add_test(
        NAME liric_cli_exe_intrinsic_math
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:liric_bin>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/intrinsic_exec_math.ll
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_exe_intrinsic_math
            -DEXPECT_RC=20
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_run.cmake
    )

    add_test(
        NAME liric_cli_exe_intrinsic_mem
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:liric_bin>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/intrinsic_exec_mem.ll
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_exe_intrinsic_mem
            -DEXPECT_RC=130
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_run.cmake
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(QEMU_RISCV64_EXE qemu-riscv64)
    if(QEMU_RISCV64_EXE)
        add_test(
            NAME liric_cli_exe_riscv64im_qemu_ret42
            COMMAND ${CMAKE_COMMAND}
                -DCLI=$<TARGET_FILE:liric_bin>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_exe_riscv64im_qemu_ret42
                -DTARGET=riscv64im
                -DQEMU=${QEMU_RISCV64_EXE}
                -DEXPECT_RC=42
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_riscv_qemu_run.cmake
        )

        add_test(
            NAME liric_cli_exe_riscv64im_qemu_add_immediates
            COMMAND ${CMAKE_COMMAND}
                -DCLI=$<TARGET_FILE:liric_bin>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/riscv_add_imm_42.ll
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_exe_riscv64im_qemu_add_immediates
                -DTARGET=riscv64im
                -DQEMU=${QEMU_RISCV64_EXE}
                -DEXPECT_RC=42
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_riscv_qemu_run.cmake
        )

        add_test(
            NAME liric_cli_exe_riscv64gc_qemu_fp
            COMMAND ${CMAKE_COMMAND}
                -DCLI=$<TARGET_FILE:liric_bin>
                -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/riscv_fp_ret_9.ll
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/ctest_work/liric_cli_exe_riscv64gc_qemu_fp
                -DTARGET=riscv64gc
                -DQEMU=${QEMU_RISCV64_EXE}
                -DEXPECT_RC=9
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_riscv_qemu_run.cmake
        )
    endif()

    add_test(
        NAME liric_cli_exe_riscv64im_rejects_fp
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:liric_bin>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/riscv_fp_ret_9.ll
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
            -DTARGET=riscv64im
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_riscv_emit_fail.cmake
    )
endif()

add_test(
    NAME bench_compat_check_freeze_artifacts
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_COMPAT_CHECK=$<TARGET_FILE:bench_compat_check>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_compat_check_freeze.cmake
)

add_test(
    NAME bench_compat_check_rc_policy
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_COMPAT_CHECK=$<TARGET_FILE:bench_compat_check>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_compat_check_rc_policy.cmake
)

add_test(
    NAME bench_ll_empty_dataset_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_LL=$<TARGET_FILE:bench_ll>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_ll_empty_dataset_gate.cmake
)

add_test(
    NAME bench_corpus_empty_dataset_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_CORPUS=$<TARGET_FILE:bench_corpus>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_corpus_empty_dataset_gate.cmake
)

add_test(
    NAME bench_exe_matrix_modes
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_EXE_MATRIX=$<TARGET_FILE:bench_exe_matrix>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_exe_matrix_modes.cmake
)

add_test(
    NAME bench_api_nonzero_compat
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_nonzero_compat.cmake
)

add_test(
    NAME bench_api_empty_dataset_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_empty_dataset_gate.cmake
)

add_test(
    NAME bench_api_signal_classification
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_signal_classification.cmake
)

add_test(
    NAME bench_api_timeout_diagnostics
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_timeout_diagnostics.cmake
)

add_test(
    NAME bench_api_llvm_retry_paths
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_llvm_retry_paths.cmake
)

add_test(
    NAME bench_api_time_report_segv_recovery
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_time_report_segv_recovery.cmake
)

add_test(
    NAME bench_api_alias_source
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_alias_source.cmake
)

add_test(
    NAME bench_api_expected_stop_classification
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_expected_stop_classification.cmake
)

add_test(
    NAME bench_api_options_hygiene
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_options_hygiene.cmake
)

add_test(
    NAME bench_api_strict_mode
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_strict_mode.cmake
)

add_test(
    NAME bench_api_zero_skip_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_zero_skip_gate.cmake
)

add_test(
    NAME bench_api_clean_gate_forwarding
    COMMAND ${CMAKE_COMMAND}
        -DGATE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_api_clean_gate.sh
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_clean_gate_forwarding.cmake
)

add_test(
    NAME nightly_mass_workflow_contract
    COMMAND ${CMAKE_COMMAND}
        -DWORKFLOW=${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/nightly_mass.yml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_nightly_mass_workflow.cmake
)

add_test(
    NAME nightly_mass_shell_runner
    COMMAND ${CMAKE_COMMAND}
        -DRUNNER=${CMAKE_CURRENT_SOURCE_DIR}/tools/lfortran_mass/nightly_mass.sh
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_nightly_mass_shell_runner.cmake
)

add_test(
    NAME llvm_backend_compat_policy
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_llvm_backend_compat_policy.cmake
)

add_test(
    NAME llvm_backend_workflow_contract
    COMMAND ${CMAKE_COMMAND}
        -DWORKFLOW=${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/llvm_backend_compat.yml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_llvm_backend_workflow_contract.cmake
)

if(CMAKE_NM AND NOT WITH_REAL_LLVM_BACKEND)
    add_test(
        NAME liric_static_archive_no_llvm_undef
        COMMAND ${CMAKE_COMMAND}
            -DNM_EXE=${CMAKE_NM}
            -DARCHIVE=$<TARGET_FILE:liric>
            -DHAVE_LLVM_BITCODE=OFF
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_static_archive_symbols.cmake
    )
endif()

include(GNUInstallDirs)
install(TARGETS liric ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/liric DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/llvm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS liric_bin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WITH_LLVM_COMPAT)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_executable(test_llvm_compat tests/test_llvm_compat.cpp)
    target_link_libraries(test_llvm_compat PRIVATE liric ${LIRIC_PLATFORM_LIBS} stdc++)
    target_compile_options(test_llvm_compat PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    add_test(NAME llvm_compat_tests COMMAND test_llvm_compat)

    if(WITH_REAL_LLVM_BACKEND)
        add_executable(test_llvm_backend_roundtrip tests/test_llvm_backend_roundtrip.cpp)
        target_link_libraries(test_llvm_backend_roundtrip PRIVATE liric ${LIRIC_PLATFORM_LIBS} stdc++)
        target_compile_options(test_llvm_backend_roundtrip PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME llvm_backend_roundtrip_tests COMMAND test_llvm_backend_roundtrip)
    endif()
endif()
