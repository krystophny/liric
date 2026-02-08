cmake_minimum_required(VERSION 3.10)
project(liric VERSION 0.1.0 LANGUAGES C)

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

if(UNIX AND NOT APPLE)
    add_link_options("$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-rdynamic>")
endif()

add_library(liric STATIC
    src/arena.c
    src/ir.c
    src/ll_lexer.c
    src/ll_parser.c
    src/wasm_decode.c
    src/wasm_to_ir.c
    src/target_registry.c
    src/target_x86_64.c
    src/target_aarch64.c
    src/jit.c
    src/liric.c
    src/builder.c
    src/liric_compat.c
    src/objfile.c
    src/objfile_macho.c
    src/objfile_elf.c
    src/llvm_stubs.c
)
target_include_directories(liric PUBLIC include)
target_include_directories(liric PRIVATE src)

add_executable(liric_bin tools/liric_main.c)
set_target_properties(liric_bin PROPERTIES OUTPUT_NAME liric)
set(LIRIC_PLATFORM_LIBS)
if(UNIX AND NOT APPLE)
    list(APPEND LIRIC_PLATFORM_LIBS dl)
endif()
if(UNIX)
    list(APPEND LIRIC_PLATFORM_LIBS m)
endif()
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

find_program(LLVM_CONFIG_EXE llvm-config)
if(LLVM_CONFIG_EXE)
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
    message(STATUS "llvm-config not found: bench_lli_phases target will not be built")
endif()

enable_testing()
add_executable(test_liric
    tests/test_main.c
    tests/test_lexer.c
    tests/test_parser.c
    tests/test_codegen.c
    tests/test_targets.c
    tests/test_jit.c
    tests/test_e2e.c
    tests/test_wasm.c
    tests/test_builder.c
    tests/test_objfile.c
)
target_link_libraries(test_liric PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(test_liric PRIVATE src)
add_test(NAME liric_tests COMMAND test_liric)

include(GNUInstallDirs)
install(TARGETS liric ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/liric DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/llvm DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS liric_bin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

option(WITH_LLVM_COMPAT "Build LLVM C++ compatibility layer tests" OFF)
if(WITH_LLVM_COMPAT)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    add_executable(test_llvm_compat tests/test_llvm_compat.cpp)
    target_link_libraries(test_llvm_compat PRIVATE liric ${LIRIC_PLATFORM_LIBS} stdc++)
    target_compile_options(test_llvm_compat PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    add_test(NAME llvm_compat_tests COMMAND test_llvm_compat)
endif()
