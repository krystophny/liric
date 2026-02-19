if(NOT DEFINED ARCHIVE OR ARCHIVE STREQUAL "")
    message(FATAL_ERROR "ARCHIVE is required")
endif()

if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "archive does not exist: ${ARCHIVE}")
endif()

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(_nm "${NM_EXE}")
if(_nm STREQUAL "")
    set(_nm nm)
endif()

function(read_def_names def_path macro_name out_var)
    if(NOT EXISTS "${def_path}")
        message(FATAL_ERROR "missing def file: ${def_path}")
    endif()
    file(STRINGS "${def_path}" _matches REGEX "^${macro_name}\\([A-Za-z0-9_]+\\)$")
    set(_names "")
    foreach(_match IN LISTS _matches)
        string(REGEX REPLACE "${macro_name}\\(([A-Za-z0-9_]+)\\)" "\\1" _name "${_match}")
        list(APPEND _names "${_name}")
    endforeach()
    set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()

read_def_names("${SOURCE_DIR}/include/llvm/Config/Targets.def" "LLVM_TARGET" target_names)
read_def_names("${SOURCE_DIR}/include/llvm/Config/AsmPrinters.def" "LLVM_ASM_PRINTER" printer_names)
read_def_names("${SOURCE_DIR}/include/llvm/Config/AsmParsers.def" "LLVM_ASM_PARSER" parser_names)

set(expected_symbols "")
foreach(_target IN LISTS target_names)
    list(APPEND expected_symbols
        "LLVMInitialize${_target}Target"
        "LLVMInitialize${_target}TargetInfo"
        "LLVMInitialize${_target}TargetMC"
    )
endforeach()
foreach(_target IN LISTS printer_names)
    list(APPEND expected_symbols "LLVMInitialize${_target}AsmPrinter")
endforeach()
foreach(_target IN LISTS parser_names)
    list(APPEND expected_symbols "LLVMInitialize${_target}AsmParser")
endforeach()

list(REMOVE_DUPLICATES expected_symbols)

execute_process(
    COMMAND "${_nm}" -g --defined-only "${ARCHIVE}"
    RESULT_VARIABLE NM_RC
    OUTPUT_VARIABLE NM_OUT
    ERROR_VARIABLE NM_ERR
)

if(NOT NM_RC EQUAL 0)
    message(FATAL_ERROR "nm failed (${NM_RC}): ${NM_ERR}")
endif()

set(missing "")
foreach(_sym IN LISTS expected_symbols)
    if(NOT NM_OUT MATCHES "([ \t\r\n]|^)_?${_sym}([ \t\r\n]|$)")
        list(APPEND missing "${_sym}")
    endif()
endforeach()

if(missing)
    string(REPLACE ";" "\n" missing_text "${missing}")
    message(FATAL_ERROR
        "missing LLVM target initialization stubs in libliric.a:\n${missing_text}")
endif()
