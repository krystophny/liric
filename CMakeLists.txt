cmake_minimum_required(VERSION 3.10)
project(liric VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

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

add_executable(bench_parse_vs_jit tools/bench_parse_vs_jit.c)
target_link_libraries(bench_parse_vs_jit PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(bench_parse_vs_jit PRIVATE src)

add_executable(bench_jit_phases tools/bench_jit_phases.c)
target_link_libraries(bench_jit_phases PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(bench_jit_phases PRIVATE src)

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
