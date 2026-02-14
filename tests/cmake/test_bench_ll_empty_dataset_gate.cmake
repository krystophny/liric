if(NOT DEFINED BENCH_LL OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_LL and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_ll_empty_dataset_gate")
set(runtime_bc "${root}/runtime.bc")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${runtime_bc}" "placeholder")

find_program(TRUE_BIN NAMES true)
if(NOT TRUE_BIN)
    message(FATAL_ERROR "Unable to locate `true` executable")
endif()

function(run_case case_name allow_empty expect_rc)
    set(bench_dir "${root}/${case_name}")
    set(compat "${bench_dir}/compat_ll.txt")
    set(summary "${bench_dir}/bench_ll_summary.json")

    file(MAKE_DIRECTORY "${bench_dir}")
    file(WRITE "${compat}" "")

    set(cmd
        "${BENCH_LL}"
        --bench-dir "${bench_dir}"
        --probe-runner "${TRUE_BIN}"
        --runtime-lib "${TRUE_BIN}"
        --runtime-bc "${runtime_bc}"
        --lli-phases "${TRUE_BIN}"
        --iters 1
        --timeout 1
    )
    if(allow_empty)
        list(APPEND cmd --allow-empty)
    endif()

    execute_process(
        COMMAND ${cmd}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )

    if(expect_rc EQUAL 0)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR "bench_ll --allow-empty should pass on empty dataset\nstdout:\n${out}\nstderr:\n${err}")
        endif()
    else()
        if(rc EQUAL 0)
            message(FATAL_ERROR "bench_ll should fail on empty dataset by default\nstdout:\n${out}\nstderr:\n${err}")
        endif()
    endif()

    if(NOT err MATCHES "EMPTY DATASET")
        message(FATAL_ERROR "stderr missing EMPTY DATASET marker\nstderr:\n${err}")
    endif()
    if(NOT EXISTS "${summary}")
        message(FATAL_ERROR "missing bench_ll_summary.json")
    endif()
    file(READ "${summary}" summary_text)
    if(NOT summary_text MATCHES "\"status\":\"EMPTY DATASET\"")
        message(FATAL_ERROR "summary missing EMPTY DATASET status:\n${summary_text}")
    endif()
    if(allow_empty)
        if(NOT summary_text MATCHES "\"allow_empty\":true")
            message(FATAL_ERROR "summary missing allow_empty=true:\n${summary_text}")
        endif()
    else()
        if(NOT summary_text MATCHES "\"allow_empty\":false")
            message(FATAL_ERROR "summary missing allow_empty=false:\n${summary_text}")
        endif()
    endif()
endfunction()

run_case("default_fail" OFF 1)
run_case("allow_empty_pass" ON 0)
