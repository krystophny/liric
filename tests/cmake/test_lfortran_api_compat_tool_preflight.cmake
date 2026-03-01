if(NOT DEFINED API_SCRIPT OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "API_SCRIPT and WORKDIR are required")
endif()

if(NOT EXISTS "${API_SCRIPT}")
    message(FATAL_ERROR "api compat script not found: ${API_SCRIPT}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping lfortran api compat tool preflight test")
    return()
endif()

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for lfortran api compat tool preflight test")
endif()

get_filename_component(api_dir "${API_SCRIPT}" DIRECTORY)
get_filename_component(repo_root "${api_dir}/../.." ABSOLUTE)
set(liric_build "${repo_root}/build")
if(NOT EXISTS "${liric_build}/liric_probe_runner" OR NOT EXISTS "${liric_build}/libliric.a")
    message(STATUS
        "missing ${liric_build}/liric_probe_runner or ${liric_build}/libliric.a; "
        "skipping lfortran api compat tool preflight test")
    return()
endif()

set(root "${WORKDIR}/lfortran_api_compat_tool_preflight_sandbox")
set(fake_bin "${root}/fake_bin")
set(fake_env_bin "${root}/fake_env_bin")
set(fake_lfortran "${root}/lfortran")
set(py_log "${root}/python_invocations.log")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${fake_bin}")
file(MAKE_DIRECTORY "${fake_env_bin}")

set(fake_python "${fake_bin}/python")
file(WRITE "${fake_python}" "#!/usr/bin/env bash
set -euo pipefail
if [[ -n \"\${LIRIC_TEST_PY_LOG:-}\" ]]; then
    printf '%s\\n' \"\$*\" >> \"\${LIRIC_TEST_PY_LOG}\"
fi
exit 0
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_python}")

set(fake_jq "${fake_bin}/jq")
file(WRITE "${fake_jq}" "#!/usr/bin/env bash
exit 127
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_jq}")

set(fake_env_runner "${fake_bin}/fake_env_runner")
file(WRITE "${fake_env_runner}" [=[#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 4 || "$1" != "run" ]]; then
    exit 2
fi
shift
if [[ "${1:-}" == "--no-capture-output" ]]; then
    shift
fi
if [[ $# -lt 3 || "$1" != "-n" ]]; then
    exit 2
fi
shift 2
if [[ -z "${LIRIC_TEST_ENV_PATH:-}" ]]; then
    echo "LIRIC_TEST_ENV_PATH is required" >&2
    exit 2
fi
if [[ "$1" == "bash" ]]; then
    if [[ -z "${LIRIC_TEST_BASH_EXE:-}" ]]; then
        echo "LIRIC_TEST_BASH_EXE is required for bash passthrough" >&2
        exit 2
    fi
    shift
    if [[ "${1:-}" == "-lc" ]]; then
        shift
        if [[ $# -lt 1 ]]; then
            echo "bash -lc requires a command string" >&2
            exit 2
        fi
        cmd="$1"
        shift
        set -- "${LIRIC_TEST_BASH_EXE}" -c "$cmd" "$@"
    else
        set -- "${LIRIC_TEST_BASH_EXE}" "$@"
    fi
fi
PATH="${LIRIC_TEST_ENV_PATH}" "$@"
]=])
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_env_runner}")

file(MAKE_DIRECTORY "${fake_lfortran}/.git")
file(MAKE_DIRECTORY "${fake_lfortran}/build-llvm/src/bin")
file(MAKE_DIRECTORY "${fake_lfortran}/build-liric/src/bin")
file(MAKE_DIRECTORY "${fake_lfortran}/integration_tests")
file(MAKE_DIRECTORY "${fake_lfortran}/src/libasr")
file(WRITE "${fake_lfortran}/src/libasr/dwarf_convert.py" "# stub\n")
file(WRITE "${fake_lfortran}/build-llvm/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
file(WRITE "${fake_lfortran}/build-liric/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
file(WRITE "${fake_lfortran}/run_tests.py" "print('stub')\n")
file(WRITE "${fake_lfortran}/integration_tests/run_tests.py" "print('stub')\n")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_lfortran}/build-llvm/src/bin/lfortran")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_lfortran}/build-liric/src/bin/lfortran")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${fake_bin}:$ENV{PATH}"
        "LIRIC_TEST_ENV_PATH=${fake_env_bin}"
        "LIRIC_TEST_BASH_EXE=${BASH_EXE}"
        "LIRIC_TEST_PY_LOG=${py_log}"
        "${BASH_EXE}" "${API_SCRIPT}"
        --workspace "${root}/ws"
        --output-root "${root}/out"
        --lfortran-dir "${fake_lfortran}"
        --env-name test-env
        --env-runner "${fake_env_runner}"
        --skip-checkout
        --skip-lfortran-build
        --run-ref-tests no
        --run-itests yes
        --workers 1
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)
if(rc EQUAL 0)
    message(FATAL_ERROR
        "tool preflight run should fail when llvm-dwarfdump is unavailable\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()
set(summary_json "${root}/out/summary.json")
if(EXISTS "${summary_json}")
    message(FATAL_ERROR
        "tool preflight failure should occur before summary generation\nsummary:\n${summary_json}\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()
string(STRIP "${err}" err_stripped)
if(NOT err_stripped STREQUAL "" AND NOT err MATCHES "missing required command.*llvm-dwarfdump")
    message(FATAL_ERROR
        "unexpected stderr for llvm-dwarfdump preflight failure\nstdout:\n${out}\nstderr:\n${err}"
    )
endif()
if(EXISTS "${py_log}")
    file(READ "${py_log}" py_log_text)
    string(STRIP "${py_log_text}" py_log_text)
    if(NOT py_log_text STREQUAL "")
        message(FATAL_ERROR
            "integration test runner should not execute python before tool preflight\nlog:\n${py_log_text}"
        )
    endif()
endif()
