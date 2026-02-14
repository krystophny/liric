if(NOT DEFINED GATE_SCRIPT OR NOT DEFINED README_FILE OR NOT DEFINED SNAPSHOT_FILE OR NOT DEFINED TABLE_FILE OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "GATE_SCRIPT, README_FILE, SNAPSHOT_FILE, TABLE_FILE, and WORKDIR are required")
endif()

if(NOT EXISTS "${GATE_SCRIPT}")
    message(FATAL_ERROR "gate script not found: ${GATE_SCRIPT}")
endif()
if(NOT EXISTS "${README_FILE}")
    message(FATAL_ERROR "README file not found: ${README_FILE}")
endif()
if(NOT EXISTS "${SNAPSHOT_FILE}")
    message(FATAL_ERROR "snapshot file not found: ${SNAPSHOT_FILE}")
endif()
if(NOT EXISTS "${TABLE_FILE}")
    message(FATAL_ERROR "table file not found: ${TABLE_FILE}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping bench_readme_perf_gate test")
    return()
endif()

set(root "${WORKDIR}/bench_readme_perf_gate")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")

set(readme_copy "${root}/README.md")
set(snapshot_copy "${root}/readme_perf_snapshot.json")
set(table_copy "${root}/readme_perf_table.md")

configure_file("${README_FILE}" "${readme_copy}" COPYONLY)
configure_file("${SNAPSHOT_FILE}" "${snapshot_copy}" COPYONLY)
configure_file("${TABLE_FILE}" "${table_copy}" COPYONLY)

execute_process(
    COMMAND "${BASH_EXE}" "${GATE_SCRIPT}"
        --readme "${readme_copy}"
        --snapshot "${snapshot_copy}"
        --table "${table_copy}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_readme_perf_gate should pass on committed artifacts\nstdout:\n${out}\nstderr:\n${err}")
endif()

if(NOT out MATCHES "bench_readme_perf_gate: PASSED")
    message(FATAL_ERROR "gate pass output missing PASSED marker\nstdout:\n${out}\nstderr:\n${err}")
endif()

set(bad_snapshot "${root}/bad_snapshot.json")
file(READ "${snapshot_copy}" snapshot_text)
string(REGEX REPLACE "[ \\t]*\"benchmark_commit\"[ \\t]*:[^\\n]*\\n" "" snapshot_text_bad "${snapshot_text}")
file(WRITE "${bad_snapshot}" "${snapshot_text_bad}")

execute_process(
    COMMAND "${BASH_EXE}" "${GATE_SCRIPT}"
        --readme "${readme_copy}"
        --snapshot "${bad_snapshot}"
        --table "${table_copy}"
    RESULT_VARIABLE bad_rc
    OUTPUT_VARIABLE bad_out
    ERROR_VARIABLE bad_err
)

if(bad_rc EQUAL 0)
    message(FATAL_ERROR "bench_readme_perf_gate should fail when benchmark_commit is missing\nstdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()

if(NOT bad_err MATCHES "benchmark_commit")
    message(FATAL_ERROR "failure output should mention benchmark_commit\nstdout:\n${bad_out}\nstderr:\n${bad_err}")
endif()
