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
)
target_include_directories(liric PUBLIC include)
target_include_directories(liric PRIVATE src)

add_executable(liric_cli tools/liric_main.c)
set(LIRIC_PLATFORM_LIBS)
if(UNIX AND NOT APPLE)
    list(APPEND LIRIC_PLATFORM_LIBS dl)
endif()
target_link_libraries(liric_cli PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(liric_cli PRIVATE src)

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
)
target_link_libraries(test_liric PRIVATE liric ${LIRIC_PLATFORM_LIBS})
target_include_directories(test_liric PRIVATE src)
add_test(NAME liric_tests COMMAND test_liric)
