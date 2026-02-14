if(NOT DEFINED BENCH_EXE_MATRIX OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_EXE_MATRIX and WORKDIR are required")
endif()

if(NOT EXISTS "${BENCH_EXE_MATRIX}")
    message(FATAL_ERROR "bench_exe_matrix executable not found: ${BENCH_EXE_MATRIX}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping bench_exe_matrix modes test")
    return()
endif()

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for bench_exe_matrix modes test")
endif()

set(root "${WORKDIR}/bench_exe_matrix_modes")
set(bench_dir "${root}/bench")
set(log_dir "${root}/logs")
set(fake_liric "${root}/fake_liric.sh")
set(fake_llvm "${root}/fake_llvm.sh")
set(modes_log "${log_dir}/modes.log")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${log_dir}")

file(WRITE "${fake_liric}" "#!/usr/bin/env bash
set -euo pipefail
out=''
input=''
while [[ $# -gt 0 ]]; do
    case \"$1\" in
        -o) out=\"$2\"; shift 2 ;;
        *) input=\"$1\"; shift 1 ;;
    esac
done
base=\"$(basename \"$input\" .ll)\"
case \"$base\" in
    ret42|add) rc=42 ;;
    arith_chain) rc=25 ;;
    loop_sum) rc=55 ;;
    fib20) rc=109 ;;
    *) rc=1 ;;
esac
echo \"$LIRIC_COMPILE_MODE\" >> \"${modes_log}\"
cat > \"$out\" <<SCRIPT
#!/usr/bin/env bash
exit $rc
SCRIPT
chmod +x \"$out\"
")

file(WRITE "${fake_llvm}" "#!/usr/bin/env bash
set -euo pipefail
out=''
input=''
while [[ $# -gt 0 ]]; do
    case \"$1\" in
        -o) out=\"$2\"; shift 2 ;;
        -O0|-Wno-override-module) shift 1 ;;
        -x) shift 2 ;;
        *) input=\"$1\"; shift 1 ;;
    esac
done
base=\"$(basename \"$input\" .ll)\"
case \"$base\" in
    ret42|add) rc=42 ;;
    arith_chain) rc=25 ;;
    loop_sum) rc=55 ;;
    fib20) rc=109 ;;
    *) rc=1 ;;
esac
cat > \"$out\" <<SCRIPT
#!/usr/bin/env bash
exit $rc
SCRIPT
chmod +x \"$out\"
")

execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_liric}" "${fake_llvm}")

execute_process(
    COMMAND "${BENCH_EXE_MATRIX}"
        --iters 1
        --bench-dir "${bench_dir}"
        --liric "${fake_liric}"
        --llvm-driver "${fake_llvm}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "bench_exe_matrix run failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

set(summary "${bench_dir}/bench_exe_matrix_summary.json")
if(NOT EXISTS "${summary}")
    message(FATAL_ERROR "missing summary artifact: ${summary}")
endif()

file(READ "${summary}" summary_text)
foreach(mode isel copy_patch llvm)
    if(NOT summary_text MATCHES "\\\"mode\\\":\\\"${mode}\\\"")
        message(FATAL_ERROR "summary missing mode ${mode}:\n${summary_text}")
    endif()
endforeach()

if(NOT summary_text MATCHES "\\\"cases_total\\\"[ \\t]*:[ \\t]*5")
    message(FATAL_ERROR "summary missing expected case count:\n${summary_text}")
endif()

if(NOT summary_text MATCHES "\\\"liric_failures\\\"[ \\t]*:[ \\t]*0")
    message(FATAL_ERROR "summary reports unexpected liric failures:\n${summary_text}")
endif()

if(NOT summary_text MATCHES "\\\"llvm_failures\\\"[ \\t]*:[ \\t]*0")
    message(FATAL_ERROR "summary reports unexpected llvm failures:\n${summary_text}")
endif()

if(NOT EXISTS "${modes_log}")
    message(FATAL_ERROR "missing modes log: ${modes_log}")
endif()

file(READ "${modes_log}" modes_text)
foreach(mode isel copy_patch llvm)
    string(REGEX MATCHALL "${mode}" mode_hits "${modes_text}")
    list(LENGTH mode_hits mode_count)
    if(NOT mode_count EQUAL 5)
        message(FATAL_ERROR "expected 5 invocations for mode=${mode}, got ${mode_count}\nlog:\n${modes_text}")
    endif()
endforeach()
