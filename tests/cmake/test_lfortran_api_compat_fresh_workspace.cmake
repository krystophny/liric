if(NOT DEFINED API_SCRIPT OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "API_SCRIPT and WORKDIR are required")
endif()

if(NOT EXISTS "${API_SCRIPT}")
    message(FATAL_ERROR "api compat script not found: ${API_SCRIPT}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping lfortran api compat fresh workspace test")
    return()
endif()

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for lfortran api compat fresh workspace test")
endif()

find_program(GIT_EXE git)
if(NOT GIT_EXE)
    message(FATAL_ERROR "git is required for lfortran api compat fresh workspace test")
endif()

get_filename_component(api_dir "${API_SCRIPT}" DIRECTORY)
get_filename_component(repo_root "${api_dir}/../.." ABSOLUTE)
set(liric_build "${repo_root}/build")
if(NOT EXISTS "${liric_build}/liric_probe_runner" OR NOT EXISTS "${liric_build}/libliric.a")
    message(STATUS
        "missing ${liric_build}/liric_probe_runner or ${liric_build}/libliric.a; "
        "skipping lfortran api compat fresh workspace test")
    return()
endif()

set(root "${WORKDIR}/lfortran_api_compat_fresh_workspace_sandbox")
set(fake_bin "${root}/fake_bin")
set(upstream_repo "${root}/upstream")
set(workspace "${root}/workspace")
set(lfortran_dir "${workspace}/lfortran")
set(output_root "${root}/out")
set(stale_marker_lfortran "${lfortran_dir}/stale.marker")
set(stale_marker_output "${output_root}/stale.marker")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${fake_bin}")

set(fake_python "${fake_bin}/python")
file(WRITE "${fake_python}" "#!/usr/bin/env bash
set -euo pipefail
exit 0
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_python}")

set(fake_jq "${fake_bin}/jq")
file(WRITE "${fake_jq}" "#!/usr/bin/env bash
exit 127
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_jq}")

file(MAKE_DIRECTORY "${upstream_repo}/integration_tests")
file(MAKE_DIRECTORY "${upstream_repo}/build-llvm/src/bin")
file(MAKE_DIRECTORY "${upstream_repo}/build-liric/src/bin")
file(WRITE "${upstream_repo}/run_tests.py" "print('stub')\n")
file(WRITE "${upstream_repo}/integration_tests/run_tests.py" "print('stub')\n")
file(WRITE "${upstream_repo}/build-llvm/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
file(WRITE "${upstream_repo}/build-liric/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${upstream_repo}/build-llvm/src/bin/lfortran")
execute_process(COMMAND "${CHMOD_EXE}" +x "${upstream_repo}/build-liric/src/bin/lfortran")

execute_process(
    COMMAND "${GIT_EXE}" init
    WORKING_DIRECTORY "${upstream_repo}"
    RESULT_VARIABLE git_init_rc
    OUTPUT_VARIABLE git_init_out
    ERROR_VARIABLE git_init_err
)
if(NOT git_init_rc EQUAL 0)
    message(FATAL_ERROR
        "failed to initialize upstream test repo rc=${git_init_rc}\nstdout:\n${git_init_out}\nstderr:\n${git_init_err}"
    )
endif()

execute_process(
    COMMAND "${GIT_EXE}" add .
    WORKING_DIRECTORY "${upstream_repo}"
    RESULT_VARIABLE git_add_rc
    OUTPUT_VARIABLE git_add_out
    ERROR_VARIABLE git_add_err
)
if(NOT git_add_rc EQUAL 0)
    message(FATAL_ERROR
        "failed to stage upstream test repo rc=${git_add_rc}\nstdout:\n${git_add_out}\nstderr:\n${git_add_err}"
    )
endif()

execute_process(
    COMMAND "${GIT_EXE}" -c user.name=liric-test -c user.email=liric-test@example.com commit -m "seed"
    WORKING_DIRECTORY "${upstream_repo}"
    RESULT_VARIABLE git_commit_rc
    OUTPUT_VARIABLE git_commit_out
    ERROR_VARIABLE git_commit_err
)
if(NOT git_commit_rc EQUAL 0)
    message(FATAL_ERROR
        "failed to commit upstream test repo rc=${git_commit_rc}\nstdout:\n${git_commit_out}\nstderr:\n${git_commit_err}"
    )
endif()

file(MAKE_DIRECTORY "${lfortran_dir}")
file(WRITE "${stale_marker_lfortran}" "stale\n")
file(MAKE_DIRECTORY "${output_root}")
file(WRITE "${stale_marker_output}" "stale\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${fake_bin}:$ENV{PATH}"
        "${BASH_EXE}" "${API_SCRIPT}"
        --workspace "${workspace}"
        --output-root "${output_root}"
        --fresh-workspace
        --lfortran-dir "${lfortran_dir}"
        --lfortran-repo "${upstream_repo}"
        --skip-checkout
        --skip-lfortran-build
        --run-ref-tests yes
        --run-itests yes
        --workers 1
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "fresh workspace run failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()

if(EXISTS "${stale_marker_lfortran}")
    message(FATAL_ERROR "fresh workspace run must remove stale lfortran workspace marker")
endif()
if(EXISTS "${stale_marker_output}")
    message(FATAL_ERROR "fresh workspace run must remove stale output root marker")
endif()
if(NOT EXISTS "${lfortran_dir}/.git")
    message(FATAL_ERROR "fresh workspace run must recreate lfortran checkout via clone")
endif()
if(NOT EXISTS "${output_root}/logs/clone.log")
    message(FATAL_ERROR "fresh workspace run must write clone.log in fresh output logs")
endif()
if(NOT EXISTS "${output_root}/summary.json")
    message(FATAL_ERROR "fresh workspace run did not produce summary.json")
endif()
file(READ "${output_root}/summary.json" summary_text)
if(NOT summary_text MATCHES "\"pass\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "fresh workspace run summary must report pass=true\nsummary:\n${summary_text}")
endif()
