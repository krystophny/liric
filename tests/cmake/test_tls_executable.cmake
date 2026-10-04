file(MAKE_DIRECTORY "${WORK_DIR}")
set(program "${WORK_DIR}/tls-executable")
execute_process(COMMAND "${TEST_TLS}" --exe "${FIXTURE}" "${program}"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "TLS executable emission failed: ${stdout}${stderr}")
endif()
file(CHMOD "${program}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
execute_process(COMMAND "${program}" TIMEOUT 20
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "TLS executable behavior failed: ${stdout}${stderr}")
endif()
message(STATUS "PASS native no-link OpenMP executable: all four TLS values preserved")
