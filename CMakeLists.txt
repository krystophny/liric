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
if(NOT LIRIC_LLVM_CONFIG_EXE)
    if(EXISTS "/opt/homebrew/opt/llvm@20/bin/llvm-config")
        set(LIRIC_LLVM_CONFIG_EXE "/opt/homebrew/opt/llvm@20/bin/llvm-config")
    elseif(EXISTS "/usr/local/opt/llvm@20/bin/llvm-config")
        set(LIRIC_LLVM_CONFIG_EXE "/usr/local/opt/llvm@20/bin/llvm-config")
    endif()
endif()
set(LIRIC_LLVM_COMPAT_MIN_MAJOR "11")
set(LIRIC_HAVE_LLVM_C_LLJIT 0)
set(LIRIC_HAVE_LLVM_ORC_LLJIT_GET_DATALAYOUT_STR 0)

set(LIRIC_REAL_LLVM_INCLUDE_DIR "")
set(LIRIC_REAL_LLVM_CFLAGS_LIST "")
set(LIRIC_REAL_LLVM_LDFLAGS_LIST "")
set(LIRIC_REAL_LLVM_LIBS_LIST "")
set(LIRIC_REAL_LLVM_SYSTEM_LIBS_LIST "")
set(LIRIC_BACKEND_LLVM_VERSION "")
set(LIRIC_BACKEND_LLVM_VERSION_MAJOR "0")

set(LIRIC_LFORTRAN_ROOT "${CMAKE_CURRENT_BINARY_DIR}/deps/lfortran"
    CACHE PATH "Path to lfortran checkout used by liric tooling")
set(LIRIC_LFORTRAN_BUILD_LLVM "${LIRIC_LFORTRAN_ROOT}/build-llvm"
    CACHE PATH "Path to lfortran baseline LLVM build directory")
set(LIRIC_LFORTRAN_BUILD_LIRIC "${LIRIC_LFORTRAN_ROOT}/build-liric"
    CACHE PATH "Path to lfortran WITH_LIRIC build directory")
set(LIRIC_LFORTRAN_API_OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/deps/lfortran_api_compat_out"
    CACHE PATH "Path to lfortran API compatibility output artifacts")
set(LIRIC_LFORTRAN_REPO "https://github.com/krystophny/lfortran.git"
    CACHE STRING "LFortran repository URL for CMake-managed checkout")
set(LIRIC_LFORTRAN_REF "origin/liric-aot"
    CACHE STRING "LFortran git ref for CMake-managed checkout")
set(LIRIC_LFORTRAN_REMOTE "origin"
    CACHE STRING "Remote name used for LFortran ref policy")
set(LIRIC_LFORTRAN_C_COMPILER ""
    CACHE FILEPATH "Optional C compiler override for CMake-managed LFortran builds")
set(LIRIC_LFORTRAN_CXX_COMPILER ""
    CACHE FILEPATH "Optional C++ compiler override for CMake-managed LFortran builds")

set(LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE "${LIRIC_LFORTRAN_C_COMPILER}")
set(LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE "${LIRIC_LFORTRAN_CXX_COMPILER}")

if(NOT LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE OR NOT LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE)
    # Homebrew LLVM static archives on macOS are built against libc++, so
    # default managed LFortran builds to clang/clang++ unless explicitly overridden.
    if(APPLE)
        find_program(_liric_clang_exe NAMES clang)
        find_program(_liric_clangxx_exe NAMES clang++)
        if(_liric_clang_exe AND _liric_clangxx_exe)
            if(NOT LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE)
                set(LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE "${_liric_clang_exe}")
            endif()
            if(NOT LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE)
                set(LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE "${_liric_clangxx_exe}")
            endif()
        endif()
    endif()

    if(NOT LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE AND DEFINED ENV{CC} AND NOT "$ENV{CC}" STREQUAL "")
        set(LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE "$ENV{CC}")
    endif()

    if(NOT LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE AND DEFINED ENV{CXX} AND NOT "$ENV{CXX}" STREQUAL "")
        set(_liric_env_cxx "$ENV{CXX}")
        set(_liric_cxx_candidate "${_liric_env_cxx}")
        if(_liric_env_cxx MATCHES "(.*/)?gcc-([0-9]+)$")
            find_program(_liric_gxx_candidate NAMES "g++-${CMAKE_MATCH_2}")
            if(_liric_gxx_candidate)
                set(_liric_cxx_candidate "${_liric_gxx_candidate}")
            endif()
        elseif(_liric_env_cxx MATCHES "(.*/)?gcc$")
            find_program(_liric_gxx_candidate NAMES g++)
            if(_liric_gxx_candidate)
                set(_liric_cxx_candidate "${_liric_gxx_candidate}")
            endif()
        endif()
        set(LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE "${_liric_cxx_candidate}")
    endif()

    if(NOT LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE AND
       LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE MATCHES "(.*/)?gcc-([0-9]+)$")
        find_program(_liric_gxx_candidate NAMES "g++-${CMAKE_MATCH_2}")
        if(_liric_gxx_candidate)
            set(LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE "${_liric_gxx_candidate}")
        endif()
    elseif(NOT LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE AND
           LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE MATCHES "(.*/)?gcc$")
        find_program(_liric_gxx_candidate NAMES g++)
        if(_liric_gxx_candidate)
            set(LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE "${_liric_gxx_candidate}")
        endif()
    endif()
endif()

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

    if(EXISTS "${LIRIC_REAL_LLVM_INCLUDE_DIR}/llvm-c/LLJIT.h")
        set(LIRIC_HAVE_LLVM_C_LLJIT 1)
        file(READ "${LIRIC_REAL_LLVM_INCLUDE_DIR}/llvm-c/LLJIT.h"
            LIRIC_LLJIT_C_HEADER)
        if(LIRIC_LLJIT_C_HEADER MATCHES "LLVMOrcLLJITGetDataLayoutStr")
            set(LIRIC_HAVE_LLVM_ORC_LLJIT_GET_DATALAYOUT_STR 1)
        endif()
        unset(LIRIC_LLJIT_C_HEADER)
    endif()

    add_compile_definitions(
        LIRIC_HAVE_REAL_LLVM_BACKEND=1
        LIRIC_HAVE_LLVM_C_LLJIT=${LIRIC_HAVE_LLVM_C_LLJIT}
        LIRIC_BACKEND_LLVM_VERSION_MAJOR=${LIRIC_BACKEND_LLVM_VERSION_MAJOR}
        LIRIC_BACKEND_LLVM_VERSION_NUM=${LIRIC_BACKEND_LLVM_VERSION_NUM}
        LIRIC_HAVE_LLVM_ORC_LLJIT_GET_DATALAYOUT_STR=${LIRIC_HAVE_LLVM_ORC_LLJIT_GET_DATALAYOUT_STR}
    )
    message(STATUS "Real LLVM backend enabled (llvm-config=${LLVM_BACKEND_CONFIG_EXE}, version=${LIRIC_BACKEND_LLVM_VERSION})")
endif()

option(WITH_LLVM_COMPAT "Build LLVM C++ compatibility layer tests" OFF)
option(LIRIC_INSTALL_COMPAT_HEADERS
    "Install deprecated liric_compat.h forwarding header"
    OFF)
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
    src/compiler.c
    src/liric_compat.c
    src/llvm_c_shim.c
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

add_executable(bench_compat_check tools/bench_compat_check.c tools/bench_common.c)
target_link_libraries(bench_compat_check PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_api tools/bench_api.c tools/bench_common.c)
target_link_libraries(bench_api PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_corpus_compare tools/bench_corpus_compare.c tools/bench_common.c)
target_link_libraries(bench_corpus_compare PRIVATE ${LIRIC_PLATFORM_LIBS})

add_executable(bench_matrix tools/bench_matrix.c tools/bench_common.c)
target_link_libraries(bench_matrix PRIVATE ${LIRIC_PLATFORM_LIBS})
target_compile_definitions(bench_matrix PRIVATE
    LIRIC_BENCH_HAVE_REAL_LLVM_BACKEND=$<BOOL:${WITH_REAL_LLVM_BACKEND}>
)

add_executable(bench_overhead_probe tools/bench_overhead_probe.c tools/bench_common.c)
target_link_libraries(bench_overhead_probe PRIVATE ${LIRIC_PLATFORM_LIBS})

option(WITH_BENCH_TCC "Build bench_tcc target (requires libtcc)" ON)
if(WITH_BENCH_TCC)
    find_path(LIRTCC_INCLUDE_DIR libtcc.h)
    find_library(LIRTCC_LIB tcc)
    if(LIRTCC_INCLUDE_DIR AND LIRTCC_LIB)
        add_executable(bench_tcc tools/bench_tcc.c tools/bench_common.c)
        target_include_directories(bench_tcc PRIVATE src "${LIRTCC_INCLUDE_DIR}")
        target_link_libraries(bench_tcc PRIVATE liric "${LIRTCC_LIB}" ${LIRIC_PLATFORM_LIBS})
    else()
        message(STATUS "libtcc not found: bench_tcc target will not be built")
    endif()
else()
    message(STATUS "bench_tcc target disabled (WITH_BENCH_TCC=OFF)")
endif()

option(WITH_BENCH_LLI_PHASES "Build bench_lli_phases when LLVM C ORC APIs are available" ON)
if(LIRIC_LLVM_CONFIG_EXE)
    set(LLVM_CONFIG_EXE "${LIRIC_LLVM_CONFIG_EXE}")
else()
    set(LLVM_CONFIG_EXE "")
    foreach(_cand
            /opt/homebrew/opt/llvm@20/bin/llvm-config
            /usr/local/opt/llvm@20/bin/llvm-config
            /opt/homebrew/opt/llvm/bin/llvm-config
            /usr/local/opt/llvm/bin/llvm-config)
        if(EXISTS "${_cand}")
            set(LLVM_CONFIG_EXE "${_cand}")
            break()
        endif()
    endforeach()
    if(NOT LLVM_CONFIG_EXE)
        find_program(LLVM_CONFIG_EXE NAMES llvm-config)
    endif()
endif()
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

if(CMAKE_CONFIGURATION_TYPES)
    set(LIRIC_EXTERNAL_BUILD_TYPE "Release")
else()
    set(LIRIC_EXTERNAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()

set(LIRIC_LFORTRAN_LLVM_COMPILER_ARGS "")
if(LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE)
    list(APPEND LIRIC_LFORTRAN_LLVM_COMPILER_ARGS
        -DCMAKE_C_COMPILER=${LIRIC_LFORTRAN_C_COMPILER_EFFECTIVE})
endif()
if(LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE)
    list(APPEND LIRIC_LFORTRAN_LLVM_COMPILER_ARGS
        -DCMAKE_CXX_COMPILER=${LIRIC_LFORTRAN_CXX_COMPILER_EFFECTIVE})
endif()

set(LIRIC_LFORTRAN_LLVM_DIR "" CACHE PATH
    "Optional LLVM_DIR override for baseline LFortran build")
set(LIRIC_LFORTRAN_LLVM_DIR_EFFECTIVE "${LIRIC_LFORTRAN_LLVM_DIR}")
if(NOT LIRIC_LFORTRAN_LLVM_DIR_EFFECTIVE AND LLVM_CONFIG_EXE)
    execute_process(
        COMMAND ${LLVM_CONFIG_EXE} --cmakedir
        OUTPUT_VARIABLE _liric_lfortran_llvm_cmakedir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _liric_lfortran_llvm_cmakedir_rc
    )
    if(_liric_lfortran_llvm_cmakedir_rc EQUAL 0 AND
       EXISTS "${_liric_lfortran_llvm_cmakedir}/LLVMConfig.cmake")
        set(LIRIC_LFORTRAN_LLVM_DIR_EFFECTIVE
            "${_liric_lfortran_llvm_cmakedir}")
    endif()
endif()

set(LIRIC_LFORTRAN_LLVM_ARGS "")
if(LIRIC_LFORTRAN_LLVM_DIR_EFFECTIVE)
    list(APPEND LIRIC_LFORTRAN_LLVM_ARGS
        -DLLVM_DIR=${LIRIC_LFORTRAN_LLVM_DIR_EFFECTIVE})
endif()

set(LIRIC_LFORTRAN_LIRIC_LINK_REAL_LLVM no CACHE BOOL
    "Allow WITH_LIRIC LFortran build to link real LLVM libraries")

set(LIRIC_LFORTRAN_LIRIC_COMPILER_ARGS "")
if(LIRIC_LFORTRAN_LIRIC_LINK_REAL_LLVM)
    list(APPEND LIRIC_LFORTRAN_LIRIC_COMPILER_ARGS
        ${LIRIC_LFORTRAN_LLVM_COMPILER_ARGS})
endif()

set(LIRIC_LFORTRAN_LIRIC_PRECONFIGURE_COMMAND
    ${CMAKE_COMMAND} -E true)
if(LIRIC_LFORTRAN_LIRIC_LINK_REAL_LLVM)
    set(LIRIC_LFORTRAN_LIRIC_PRECONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E rm -rf ${LIRIC_LFORTRAN_BUILD_LIRIC})
endif()

add_custom_target(
    lfortran_prepare
    COMMAND ${CMAKE_COMMAND}
        -DLFORTRAN_ROOT=${LIRIC_LFORTRAN_ROOT}
        -DLFORTRAN_REPO=${LIRIC_LFORTRAN_REPO}
        -DLFORTRAN_REF=${LIRIC_LFORTRAN_REF}
        -DLFORTRAN_REMOTE=${LIRIC_LFORTRAN_REMOTE}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_lfortran_checkout.cmake
    COMMENT "Prepare LFortran checkout in ${LIRIC_LFORTRAN_ROOT}"
    USES_TERMINAL
)

add_custom_target(
    lfortran_build_llvm
    COMMAND ${CMAKE_COMMAND}
        -S ${LIRIC_LFORTRAN_ROOT}
        -B ${LIRIC_LFORTRAN_BUILD_LLVM}
        -G Ninja
        -DCMAKE_BUILD_TYPE=${LIRIC_EXTERNAL_BUILD_TYPE}
        ${LIRIC_LFORTRAN_LLVM_COMPILER_ARGS}
        ${LIRIC_LFORTRAN_LLVM_ARGS}
        -DBUILD_TESTING=OFF
        -DWITH_LLVM=yes
    COMMAND ${CMAKE_COMMAND} --build ${LIRIC_LFORTRAN_BUILD_LLVM}
        --target lfortran lfortran_runtime --parallel
    DEPENDS lfortran_prepare
    COMMENT "Configure/build LFortran baseline LLVM in ${LIRIC_LFORTRAN_BUILD_LLVM}"
    USES_TERMINAL
)

add_custom_target(
    lfortran_build_liric
    COMMAND ${LIRIC_LFORTRAN_LIRIC_PRECONFIGURE_COMMAND}
    COMMAND ${CMAKE_COMMAND}
        -S ${LIRIC_LFORTRAN_ROOT}
        -B ${LIRIC_LFORTRAN_BUILD_LIRIC}
        -G Ninja
        -DCMAKE_BUILD_TYPE=${LIRIC_EXTERNAL_BUILD_TYPE}
        ${LIRIC_LFORTRAN_LIRIC_COMPILER_ARGS}
        -DBUILD_TESTING=OFF
        -DWITH_LIRIC=yes
        -DLIRIC_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DLIRIC_REQUIRE_REAL_LLVM_LINK=${LIRIC_LFORTRAN_LIRIC_LINK_REAL_LLVM}
        ${LIRIC_LFORTRAN_LLVM_ARGS}
        -DWITH_RUNTIME_STACKTRACE=yes
        -DWITH_LLVM=OFF
    COMMAND ${CMAKE_COMMAND} --build ${LIRIC_LFORTRAN_BUILD_LIRIC}
        --target lfortran lfortran_runtime --parallel
    DEPENDS lfortran_prepare liric_probe_runner
    COMMENT "Configure/build LFortran WITH_LIRIC in ${LIRIC_LFORTRAN_BUILD_LIRIC}"
    USES_TERMINAL
)

add_custom_target(
    lfortran_build_all
    DEPENDS lfortran_build_llvm lfortran_build_liric
)

add_custom_target(
    lfortran_api_compat
    COMMAND ${CMAKE_COMMAND} -E env
        LIRIC_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
        bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/lfortran_mass/lfortran_api_compat.sh
        --workspace ${CMAKE_CURRENT_BINARY_DIR}/deps
        --output-root ${LIRIC_LFORTRAN_API_OUTPUT_ROOT}
        --lfortran-dir ${LIRIC_LFORTRAN_ROOT}
        --skip-checkout
        --skip-lfortran-build
        --run-ref-tests yes
        --run-itests yes
    DEPENDS lfortran_build_all
    COMMENT "Run LFortran API compatibility suite using CMake-managed builds"
    USES_TERMINAL
)

add_custom_target(
    source_tree_purge_generated ALL
    COMMAND ${CMAKE_COMMAND}
        -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DMODE=purge
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_source_tree_generated_artifacts.cmake
    COMMENT "Purge generated artifacts leaked into source tree"
)

enable_testing()
set(LIRIC_CTEST_WORK_ROOT "${CMAKE_CURRENT_BINARY_DIR}/ctest_work")
file(MAKE_DIRECTORY "${LIRIC_CTEST_WORK_ROOT}")

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
    tests/test_llvm_c_shim.c
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
        -DOUT=${LIRIC_CTEST_WORK_ROOT}/liric_cli_emit_obj_test.o
        -DMODE=auto_object
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

add_test(
    NAME liric_cli_output_flag_o_suffix_forces_obj
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DOUT=${LIRIC_CTEST_WORK_ROOT}/liric_cli_emit_obj_forced.o
        -DMODE=force_object_suffix
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

add_test(
    NAME liric_cli_default_emits_exe
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_default_emits_exe
        -DMODE=default
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_mode.cmake
)

add_test(
    NAME liric_cli_output_flag_emits_exe
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_output_flag_emits_exe
        -DOUT=${LIRIC_CTEST_WORK_ROOT}/liric_cli_output_flag_emits_exe/liric_cli_custom_out
        -DMODE=output
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_mode.cmake
)

add_test(
    NAME liric_cli_output_flag_jit_conflict
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DOUT=${LIRIC_CTEST_WORK_ROOT}/liric_cli_emit_obj_conflict.o
        -DMODE=conflict
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

add_test(
    NAME liric_cli_legacy_emit_obj_rejected
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:liric_bin>
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/ret_42.ll
        -DOUT=${LIRIC_CTEST_WORK_ROOT}/liric_cli_emit_obj_legacy.o
        -DMODE=legacy_flag_rejected
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_mode.cmake
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|aarch64|arm64")
    add_test(
        NAME liric_cli_obj_powi_negative_exponent_link
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:liric_bin>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/intrinsic_obj_powi_neg.ll
            -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_obj_powi_negative_exponent_link
            -DCC=${CMAKE_C_COMPILER}
            -DNM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_obj_link_run.cmake
    )

    add_test(
        NAME liric_cli_exe_intrinsic_math
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:liric_bin>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/intrinsic_exec_math.ll
            -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_exe_intrinsic_math
            -DEXPECT_RC=20
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_exe_run.cmake
    )

    add_test(
        NAME liric_cli_exe_intrinsic_mem
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:liric_bin>
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ll/intrinsic_exec_mem.ll
            -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_exe_intrinsic_mem
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
                -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_exe_riscv64im_qemu_ret42
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
                -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_exe_riscv64im_qemu_add_immediates
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
                -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}/liric_cli_exe_riscv64gc_qemu_fp
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
            -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
            -DTARGET=riscv64im
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_liric_cli_riscv_emit_fail.cmake
    )
endif()

add_test(
    NAME bench_compat_check_freeze_artifacts
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_COMPAT_CHECK=$<TARGET_FILE:bench_compat_check>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_compat_check_freeze.cmake
)

add_test(
    NAME bench_compat_check_rc_policy
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_COMPAT_CHECK=$<TARGET_FILE:bench_compat_check>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_compat_check_rc_policy.cmake
)

add_test(
    NAME bench_corpus_compare_empty_dataset_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_CORPUS_COMPARE=$<TARGET_FILE:bench_corpus_compare>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_corpus_compare_empty_dataset_gate.cmake
)

add_test(
    NAME bench_matrix_help
    COMMAND $<TARGET_FILE:bench_matrix> --help
)

add_test(
    NAME bench_matrix_lfortran_rebuild
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_MATRIX=$<TARGET_FILE:bench_matrix>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_matrix_lfortran_rebuild.cmake
)

add_test(
    NAME bench_matrix_requires_lfortran_liric
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_MATRIX=$<TARGET_FILE:bench_matrix>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_matrix_requires_lfortran_liric.cmake
)

add_test(
    NAME bench_api_nonzero_compat
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_nonzero_compat.cmake
)

add_test(
    NAME bench_api_subprocess_overhead_summary
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_subprocess_overhead_summary.cmake
)

add_test(
    NAME bench_api_empty_dataset_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_empty_dataset_gate.cmake
)

add_test(
    NAME bench_api_requires_lfortran_liric
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_requires_lfortran_liric.cmake
)

add_test(
    NAME bench_api_signal_classification
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_signal_classification.cmake
)

add_test(
    NAME bench_api_timeout_diagnostics
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_timeout_diagnostics.cmake
)

add_test(
    NAME bench_api_llvm_retry_paths
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_llvm_retry_paths.cmake
)

add_test(
    NAME bench_api_time_report_segv_recovery
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_time_report_segv_recovery.cmake
)

add_test(
    NAME bench_api_alias_source
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_alias_source.cmake
)

add_test(
    NAME bench_api_expected_stop_classification
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_expected_stop_classification.cmake
)

add_test(
    NAME bench_api_options_hygiene
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_options_hygiene.cmake
)

add_test(
    NAME bench_api_strict_mode
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_strict_mode.cmake
)

add_test(
    NAME bench_api_zero_skip_gate
    COMMAND ${CMAKE_COMMAND}
        -DBENCH_API=$<TARGET_FILE:bench_api>
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_zero_skip_gate.cmake
)

add_test(
    NAME bench_api_clean_gate_forwarding
    COMMAND ${CMAKE_COMMAND}
        -DGATE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_api_clean_gate.sh
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_api_clean_gate_forwarding.cmake
)

add_test(
    NAME bench_readme_perf_gate
    COMMAND ${CMAKE_COMMAND}
        -DGATE_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/bench_readme_perf_gate.sh
        -DREADME_FILE=${CMAKE_CURRENT_SOURCE_DIR}/README.md
        -DSNAPSHOT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/docs/benchmarks/readme_perf_snapshot.json
        -DTABLE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/docs/benchmarks/readme_perf_table.md
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_bench_readme_perf_gate.cmake
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
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_nightly_mass_shell_runner.cmake
)

add_test(
    NAME llvm_backend_compat_policy
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_llvm_backend_compat_policy.cmake
)

add_test(
    NAME llvm_backend_workflow_contract
    COMMAND ${CMAKE_COMMAND}
        -DWORKFLOW=${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/llvm_backend_compat.yml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_llvm_backend_workflow_contract.cmake
)

add_test(
    NAME lfortran_api_compat_ref_policy
    COMMAND ${CMAKE_COMMAND}
        -DAPI_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/lfortran_mass/lfortran_api_compat.sh
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_lfortran_api_compat_ref_policy.cmake
)

add_test(
    NAME lfortran_api_compat_fresh_workspace
    COMMAND ${CMAKE_COMMAND}
        -DAPI_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tools/lfortran_mass/lfortran_api_compat.sh
        -DWORKDIR=${LIRIC_CTEST_WORK_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_lfortran_api_compat_fresh_workspace.cmake
)

add_test(
    NAME lfortran_api_compat_workflow_contract
    COMMAND ${CMAKE_COMMAND}
        -DWORKFLOW=${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/lfortran_api_compat.yml
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_lfortran_api_compat_workflow_contract.cmake
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
    add_test(
        NAME llvm_target_init_stubs_defined
        COMMAND ${CMAKE_COMMAND}
            -DNM_EXE=${CMAKE_NM}
            -DARCHIVE=$<TARGET_FILE:liric>
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_llvm_target_init_stubs.cmake
    )
endif()

include(GNUInstallDirs)
install(TARGETS liric ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES
    include/liric/liric.h
    include/liric/liric_legacy.h
    include/liric/liric_ir_shared.h
    include/liric/liric_types.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/liric
)
if(LIRIC_INSTALL_COMPAT_HEADERS)
    install(FILES
        include/liric/liric_compat.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/liric
    )
endif()
install(DIRECTORY include/llvm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/llvm-c DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS liric_bin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WITH_LLVM_COMPAT)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_executable(test_llvm_compat tests/test_llvm_compat.cpp)
    target_link_libraries(test_llvm_compat PRIVATE liric ${LIRIC_PLATFORM_LIBS} stdc++)
    target_compile_options(test_llvm_compat PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
        -Wno-error=attributes
    )
    add_test(NAME llvm_compat_tests COMMAND test_llvm_compat)

    if(WITH_REAL_LLVM_BACKEND)
        add_executable(test_llvm_backend_roundtrip tests/test_llvm_backend_roundtrip.cpp)
        target_link_libraries(test_llvm_backend_roundtrip PRIVATE liric ${LIRIC_PLATFORM_LIBS} stdc++)
        target_compile_options(test_llvm_backend_roundtrip PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-error=attributes
        )
        add_test(NAME llvm_backend_roundtrip_tests COMMAND test_llvm_backend_roundtrip)
    endif()
endif()

# Strict test sandbox policy:
# - every test runs in an isolated work directory under ${LIRIC_CTEST_WORK_ROOT}
# - source tree is purged before test run and verified clean after test run
get_property(LIRIC_ALL_TESTS DIRECTORY PROPERTY TESTS)
foreach(LIRIC_TEST_NAME IN LISTS LIRIC_ALL_TESTS)
    string(MAKE_C_IDENTIFIER "${LIRIC_TEST_NAME}" LIRIC_TEST_DIR_ID)
    set(LIRIC_TEST_WORK_DIR "${LIRIC_CTEST_WORK_ROOT}/${LIRIC_TEST_DIR_ID}")
    file(MAKE_DIRECTORY "${LIRIC_TEST_WORK_DIR}")
    set_tests_properties("${LIRIC_TEST_NAME}" PROPERTIES
        WORKING_DIRECTORY "${LIRIC_TEST_WORK_DIR}"
    )
    set_property(TEST "${LIRIC_TEST_NAME}" APPEND PROPERTY
        FIXTURES_REQUIRED liric_source_tree_sandbox
    )
endforeach()

add_test(
    NAME source_tree_generated_artifacts_purge_before
    COMMAND ${CMAKE_COMMAND}
        -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DMODE=purge
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_source_tree_generated_artifacts.cmake
)
set_tests_properties(source_tree_generated_artifacts_purge_before PROPERTIES
    FIXTURES_SETUP liric_source_tree_sandbox
    WORKING_DIRECTORY "${LIRIC_CTEST_WORK_ROOT}/source_tree_generated_artifacts_purge_before"
)
file(MAKE_DIRECTORY "${LIRIC_CTEST_WORK_ROOT}/source_tree_generated_artifacts_purge_before")

add_test(
    NAME source_tree_generated_artifacts_guard
    COMMAND ${CMAKE_COMMAND}
        -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -DMODE=verify
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_source_tree_generated_artifacts.cmake
)
set_tests_properties(source_tree_generated_artifacts_guard PROPERTIES
    FIXTURES_CLEANUP liric_source_tree_sandbox
    WORKING_DIRECTORY "${LIRIC_CTEST_WORK_ROOT}/source_tree_generated_artifacts_guard"
)
file(MAKE_DIRECTORY "${LIRIC_CTEST_WORK_ROOT}/source_tree_generated_artifacts_guard")
