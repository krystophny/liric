if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED WORKDIR OR NOT DEFINED MODE)
    message(FATAL_ERROR "CLI, INPUT, WORKDIR, and MODE are required")
endif()

if(MODE STREQUAL "default")
    set(EXE "${WORKDIR}/a.out")
    file(REMOVE "${EXE}")

    execute_process(
        COMMAND "${CLI}" "${INPUT}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "default mode failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    if(NOT EXISTS "${EXE}")
        message(FATAL_ERROR "default mode did not create executable: ${EXE}")
    endif()

    execute_process(
        COMMAND "${EXE}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE run_rc
        OUTPUT_VARIABLE run_out
        ERROR_VARIABLE run_err
    )
    if(NOT run_rc EQUAL 42)
        message(FATAL_ERROR "default executable returned ${run_rc}, expected 42\nstdout:\n${run_out}\nstderr:\n${run_err}")
    endif()

    file(REMOVE "${EXE}")
elseif(MODE STREQUAL "output")
    if(NOT DEFINED OUT)
        message(FATAL_ERROR "OUT is required for MODE=output")
    endif()
    file(REMOVE "${OUT}")

    execute_process(
        COMMAND "${CLI}" -o "${OUT}" "${INPUT}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "output mode failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    if(NOT EXISTS "${OUT}")
        message(FATAL_ERROR "output mode did not create executable: ${OUT}")
    endif()

    execute_process(
        COMMAND "${OUT}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE run_rc
        OUTPUT_VARIABLE run_out
        ERROR_VARIABLE run_err
    )
    if(NOT run_rc EQUAL 42)
        message(FATAL_ERROR "custom executable returned ${run_rc}, expected 42\nstdout:\n${run_out}\nstderr:\n${run_err}")
    endif()

    file(REMOVE "${OUT}")
else()
    message(FATAL_ERROR "Unknown MODE=${MODE}")
endif()
