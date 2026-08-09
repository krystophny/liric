if(NOT DEFINED BUILD_DIR OR NOT DEFINED PREFIX OR NOT DEFINED C_COMPILER)
    message(FATAL_ERROR "BUILD_DIR, PREFIX, and C_COMPILER are required")
endif()

file(REMOVE_RECURSE "${PREFIX}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}"
    RESULT_VARIABLE install_rc
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_rc EQUAL 0)
    message(FATAL_ERROR
        "CMake install failed (rc=${install_rc}):\n"
        "${install_stdout}\n${install_stderr}")
endif()

set(header "${PREFIX}/include/liric/liric_session.h")
if(NOT EXISTS "${header}")
    message(FATAL_ERROR "installed public session header is missing: ${header}")
endif()

set(probe_dir "${PREFIX}/install-oracle")
file(MAKE_DIRECTORY "${probe_dir}")
file(WRITE "${probe_dir}/probe.c"
    "#include <liric/liric_session.h>\n"
    "int main(void) { return 0; }\n")
execute_process(
    COMMAND "${C_COMPILER}" -std=c11 -fsyntax-only
        "-I${PREFIX}/include" "${probe_dir}/probe.c"
    RESULT_VARIABLE probe_rc
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr
)
if(NOT probe_rc EQUAL 0)
    message(FATAL_ERROR
        "installed public session header did not compile (rc=${probe_rc}):\n"
        "${probe_stdout}\n${probe_stderr}")
endif()

message(STATUS "installed public session header compiles: ${header}")
