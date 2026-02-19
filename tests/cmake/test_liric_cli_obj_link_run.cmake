if(NOT DEFINED CLI OR NOT DEFINED INPUT OR NOT DEFINED WORKDIR OR NOT DEFINED CC)
    message(FATAL_ERROR "CLI, INPUT, WORKDIR, and CC are required")
endif()

file(MAKE_DIRECTORY "${WORKDIR}")

set(OBJ "${WORKDIR}/intrinsic_obj_powi_neg.o")
set(EXE "${WORKDIR}/intrinsic_obj_powi_neg.out")
file(REMOVE "${OBJ}" "${EXE}")

execute_process(
    COMMAND "${CLI}" -o "${OBJ}" "${INPUT}"
    WORKING_DIRECTORY "${WORKDIR}"
    RESULT_VARIABLE emit_rc
    OUTPUT_VARIABLE emit_out
    ERROR_VARIABLE emit_err
)
if(NOT emit_rc EQUAL 0)
    message(FATAL_ERROR "object emission failed rc=${emit_rc}\nstdout:\n${emit_out}\nstderr:\n${emit_err}")
endif()

if(NOT EXISTS "${OBJ}")
    message(FATAL_ERROR "object was not created: ${OBJ}")
endif()

set(NM_TOOL "")
if(DEFINED NM AND NOT NM STREQUAL "")
    set(NM_TOOL "${NM}")
else()
    find_program(NM_TOOL nm)
endif()

if(NM_TOOL)
    execute_process(
        COMMAND "${NM_TOOL}" -u "${OBJ}"
        WORKING_DIRECTORY "${WORKDIR}"
        RESULT_VARIABLE nm_rc
        OUTPUT_VARIABLE nm_out
        ERROR_VARIABLE nm_err
    )
    if(NOT nm_rc EQUAL 0)
        message(FATAL_ERROR "nm -u failed rc=${nm_rc}\nstdout:\n${nm_out}\nstderr:\n${nm_err}")
    endif()
    if(nm_out MATCHES "([\\n]|^)[[:space:]]*U[[:space:]]+pow(f)?([\\n]|$)")
        message(FATAL_ERROR "powi intrinsic was remapped to libc pow/powf:\n${nm_out}")
    endif()
endif()

execute_process(
    COMMAND "${CC}" "${OBJ}" -lm -o "${EXE}"
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
    OUTPUT_VARIABLE run_out
    ERROR_VARIABLE run_err
)
if(NOT run_rc EQUAL 0)
    message(FATAL_ERROR "executable returned ${run_rc}, expected 0\nstdout:\n${run_out}\nstderr:\n${run_err}")
endif()
