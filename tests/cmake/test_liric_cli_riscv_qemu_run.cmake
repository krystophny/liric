if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED WORKDIR OR NOT DEFINED TARGET OR
   NOT DEFINED EXPECT_RC OR NOT DEFINED QEMU)
    message(FATAL_ERROR "CLI, INPUT, WORKDIR, TARGET, EXPECT_RC, and QEMU are required")
endif()

set(EXE "${WORKDIR}/liric_riscv_test.out")
file(REMOVE "${EXE}")

execute_process(
    COMMAND "${CLI}" "--target" "${TARGET}" "-o" "${EXE}" "${INPUT}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "emit executable failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "executable was not created: ${EXE}")
endif()

execute_process(
    COMMAND "${QEMU}" "${EXE}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE run_rc
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_err
)
if(NOT run_rc EQUAL EXPECT_RC)
    message(FATAL_ERROR "qemu run returned ${run_rc}, expected ${EXPECT_RC}\nstdout:\n${run_out}\nstderr:\n${run_err}")
endif()

file(REMOVE "${EXE}")
