if(HAVE_LLVM_BITCODE)
    message(STATUS "Skipping LLVM symbol check: bitcode decoder is enabled")
    return()
endif()

if(NOT DEFINED ARCHIVE OR ARCHIVE STREQUAL "")
    message(FATAL_ERROR "ARCHIVE is required")
endif()

if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "archive does not exist: ${ARCHIVE}")
endif()

set(_nm "${NM_EXE}")
if(_nm STREQUAL "")
    set(_nm nm)
endif()

execute_process(
    COMMAND "${_nm}" -u "${ARCHIVE}"
    RESULT_VARIABLE NM_RC
    OUTPUT_VARIABLE NM_OUT
    ERROR_VARIABLE NM_ERR
)

if(NOT NM_RC EQUAL 0)
    message(FATAL_ERROR "nm failed (${NM_RC}): ${NM_ERR}")
endif()

string(REGEX MATCH "(^|[ \t\r\n])_?LLVM[A-Za-z0-9_]+" LLVM_UNDEF_MATCH "${NM_OUT}")
if(LLVM_UNDEF_MATCH)
    string(STRIP "${LLVM_UNDEF_MATCH}" LLVM_UNDEF_MATCH)
    message(FATAL_ERROR "unexpected unresolved LLVM symbol in libliric.a: ${LLVM_UNDEF_MATCH}")
endif()
