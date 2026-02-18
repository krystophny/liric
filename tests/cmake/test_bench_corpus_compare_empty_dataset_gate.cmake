if(NOT DEFINED BENCH_CORPUS_COMPARE OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_CORPUS_COMPARE and WORKDIR are required")
endif()

set(root "${WORKDIR}/bench_corpus_compare_empty_dataset_gate")
set(cache_dir "${root}/cache")
set(corpus "${root}/corpus.tsv")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${corpus}" "case_001\tfake_case\t123\n")

find_program(TRUE_BIN NAMES true)
if(NOT TRUE_BIN)
    message(FATAL_ERROR "Unable to locate `true` executable")
endif()

function(run_case allow_empty expect_rc)
    set(bench_dir "${root}/bench")
    set(summary "${bench_dir}/bench_corpus_compare_summary.json")

    set(cmd
        "${BENCH_CORPUS_COMPARE}"
        --probe-runner "${TRUE_BIN}"
        --lli-phases "${TRUE_BIN}"
        --corpus "${corpus}"
        --cache-dir "${cache_dir}"
        --bench-dir "${bench_dir}"
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
            message(FATAL_ERROR "bench_corpus_compare --allow-empty should pass on empty dataset\nstdout:\n${out}\nstderr:\n${err}")
        endif()
    else()
        if(rc EQUAL 0)
            message(FATAL_ERROR "bench_corpus_compare should fail on empty dataset by default\nstdout:\n${out}\nstderr:\n${err}")
        endif()
    endif()

    if(NOT err MATCHES "EMPTY DATASET")
        message(FATAL_ERROR "stderr missing EMPTY DATASET marker\nstderr:\n${err}")
    endif()
    if(NOT err MATCHES "nightly_mass\\.sh")
        message(FATAL_ERROR "stderr missing cache bootstrap guidance\nstderr:\n${err}")
    endif()
    if(NOT err MATCHES "--cache-dir PATH")
        message(FATAL_ERROR "stderr missing --cache-dir guidance\nstderr:\n${err}")
    endif()
    if(NOT out MATCHES "Status: EMPTY DATASET")
        message(FATAL_ERROR "stdout missing EMPTY DATASET status\nstdout:\n${out}")
    endif()
    if(NOT EXISTS "${summary}")
        message(FATAL_ERROR "missing bench_corpus_compare_summary.json")
    endif()
    file(READ "${summary}" summary_text)
    if(NOT summary_text MATCHES "\"status\":\"EMPTY DATASET\"")
        message(FATAL_ERROR "summary missing EMPTY DATASET status:\n${summary_text}")
    endif()
endfunction()

run_case(OFF 1)
run_case(ON 0)
