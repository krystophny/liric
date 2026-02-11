if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED WORKDIR OR NOT DEFINED TARGET)
    message(FATAL_ERROR "CLI, INPUT, WORKDIR, and TARGET are required")
endif()

set(EXE "${WORKDIR}/liric_riscv_fail_test.out")
file(REMOVE "${EXE}")

execute_process(
    COMMAND "${CLI}" "--target" "${TARGET}" "-o" "${EXE}" "${INPUT}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(rc EQUAL 0)
    message(FATAL_ERROR "emit unexpectedly succeeded for target=${TARGET}\nstdout:\n${out}\nstderr:\n${err}")
endif()

string(FIND "${err}" "executable emission failed" emsg_pos)
if(emsg_pos LESS 0)
    message(FATAL_ERROR "expected executable emission failure message\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(EXISTS "${EXE}")
    file(REMOVE "${EXE}")
endif()
