if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED WORKDIR OR NOT DEFINED CC OR NOT DEFINED EXPECT_RC)
    message(FATAL_ERROR "CLI, INPUT, WORKDIR, CC, and EXPECT_RC are required")
endif()

# Emit an object through the real LLVM backend (LIRIC_COMPILE_MODE=llvm) and
# link+run it. The object path serializes the session module directly
# (module_to_ll_text), with no lr_module_merge in between, so a duplicate
# runtime declaration in the input reaches the LLVM parser verbatim -- this is
# exactly the surface of issue 522. It only succeeds because liric dedups
# redundant declarations.

file(MAKE_DIRECTORY "${WORKDIR}")

set(OBJ "${WORKDIR}/out.o")
set(EXE "${WORKDIR}/out")
file(REMOVE "${OBJ}" "${EXE}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env LIRIC_COMPILE_MODE=llvm "${CLI}" "${INPUT}" -o "${OBJ}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "llvm-backend object emit failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()
if(NOT EXISTS "${OBJ}")
    message(FATAL_ERROR "object was not created: ${OBJ}")
endif()

execute_process(
    COMMAND "${CC}" "${OBJ}" -o "${EXE}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE link_rc
    OUTPUT_VARIABLE link_out
    ERROR_VARIABLE link_err
)
if(NOT link_rc EQUAL 0)
    message(FATAL_ERROR "link failed rc=${link_rc}\nstdout:\n${link_out}\nstderr:\n${link_err}")
endif()

execute_process(
    COMMAND "${EXE}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE run_rc
)
if(NOT run_rc EQUAL EXPECT_RC)
    message(FATAL_ERROR "executable returned ${run_rc}, expected ${EXPECT_RC}")
endif()

file(REMOVE "${OBJ}" "${EXE}")
