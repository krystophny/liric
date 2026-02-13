if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED OUT OR NOT DEFINED MODE)
    message(FATAL_ERROR "CLI, INPUT, OUT, and MODE are required")
endif()

file(REMOVE "${OUT}")

if(MODE STREQUAL "auto_object")
    execute_process(
        COMMAND "${CLI}" -o "${OUT}" "${INPUT}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "emit mode failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    if(NOT EXISTS "${OUT}")
        message(FATAL_ERROR "emit mode did not create object file: ${OUT}")
    endif()
    file(SIZE "${OUT}" obj_size)
    if(obj_size LESS 64)
        message(FATAL_ERROR "emitted object looks too small (${obj_size} bytes)")
    endif()
elseif(MODE STREQUAL "conflict")
    execute_process(
        COMMAND "${CLI}" -o "${OUT}" --jit "${INPUT}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(rc EQUAL 0)
        message(FATAL_ERROR "conflict mode should fail but succeeded\nstdout:\n${out}\nstderr:\n${err}")
    endif()
elseif(MODE STREQUAL "legacy_flag_rejected")
    execute_process(
        COMMAND "${CLI}" --emit-obj "${OUT}" "${INPUT}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    if(rc EQUAL 0)
        message(FATAL_ERROR "legacy flag should fail but succeeded\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    string(FIND "${err}" "unknown option: --emit-obj" unknown_opt_pos)
    if(unknown_opt_pos EQUAL -1)
        message(FATAL_ERROR "legacy flag stderr did not mention unknown option\nstderr:\n${err}")
    endif()
else()
    message(FATAL_ERROR "Unknown MODE=${MODE}")
endif()
