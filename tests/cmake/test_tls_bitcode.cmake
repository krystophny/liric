file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(COMMAND "${LLVM_AS}" "${FIXTURE}" -o "${WORK_DIR}/tls.bc"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "TLS bitcode assembly failed: ${stdout}${stderr}")
endif()
execute_process(COMMAND "${TEST_TLS}" --bitcode "${WORK_DIR}/tls.bc"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "TLS bitcode behavior failed: ${stdout}${stderr}")
endif()
message(STATUS "${stdout}")
