if(NOT DEFINED API_SCRIPT OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "API_SCRIPT and WORKDIR are required")
endif()

if(NOT EXISTS "${API_SCRIPT}")
    message(FATAL_ERROR "api compat script not found: ${API_SCRIPT}")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping lfortran api compat reference policy test")
    return()
endif()

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for lfortran api compat reference policy test")
endif()

get_filename_component(api_dir "${API_SCRIPT}" DIRECTORY)
get_filename_component(repo_root "${api_dir}/../.." ABSOLUTE)
set(liric_build "${repo_root}/build")
if(NOT EXISTS "${liric_build}/liric_probe_runner" OR NOT EXISTS "${liric_build}/libliric.a")
    message(STATUS
        "missing ${liric_build}/liric_probe_runner or ${liric_build}/libliric.a; "
        "skipping lfortran api compat reference policy test")
    return()
endif()

set(root "${WORKDIR}/lfortran_api_compat_ref_policy_sandbox")
set(fake_bin "${root}/fake_bin")
set(default_lfortran "${root}/lfortran_default")
set(override_lfortran "${root}/lfortran_override")
set(default_log "${root}/python_default_args.log")
set(override_log "${root}/python_override_args.log")
set(ctest_log "${root}/ctest_args.log")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${fake_bin}")

set(fake_python "${fake_bin}/python")
file(WRITE "${fake_python}" "#!/usr/bin/env bash
set -euo pipefail
if [[ -n \"\${LIRIC_TEST_PY_LOG:-}\" ]]; then
    printf 'ARGS: %s\\n' \"\$*\" >> \"\${LIRIC_TEST_PY_LOG}\"
    printf 'LIRIC_REF_SKIP_IR: %s\\n' \"\${LIRIC_REF_SKIP_IR:-}\" >> \"\${LIRIC_TEST_PY_LOG}\"
    printf 'LIRIC_REF_SKIP_DBG: %s\\n' \"\${LIRIC_REF_SKIP_DBG:-}\" >> \"\${LIRIC_TEST_PY_LOG}\"
fi
exit 0
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_python}")

set(fake_jq "${fake_bin}/jq")
file(WRITE "${fake_jq}" "#!/usr/bin/env bash
exit 127
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_jq}")

set(fake_ctest "${fake_bin}/ctest")
file(WRITE "${fake_ctest}" "#!/usr/bin/env bash
set -euo pipefail
if [[ -n \"\${LIRIC_TEST_CTEST_LOG:-}\" ]]; then
    printf '%s\\n' \"\$*\" >> \"\${LIRIC_TEST_CTEST_LOG}\"
fi
exit 0
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_ctest}")

function(make_fake_lfortran_tree path)
    file(MAKE_DIRECTORY "${path}/.git")
    file(MAKE_DIRECTORY "${path}/build-llvm/src/bin")
    file(MAKE_DIRECTORY "${path}/build-liric/src/bin")
    file(MAKE_DIRECTORY "${path}/integration_tests")
    file(WRITE "${path}/build-llvm/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
    file(WRITE "${path}/build-liric/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
    file(WRITE "${path}/run_tests.py" "print('stub')\n")
    file(WRITE "${path}/integration_tests/run_tests.py" "print('stub')\n")
    execute_process(COMMAND "${CHMOD_EXE}" +x "${path}/build-llvm/src/bin/lfortran")
    execute_process(COMMAND "${CHMOD_EXE}" +x "${path}/build-liric/src/bin/lfortran")
endfunction()

make_fake_lfortran_tree("${default_lfortran}")
make_fake_lfortran_tree("${override_lfortran}")

file(REMOVE "${default_log}")
file(REMOVE "${ctest_log}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${fake_bin}:$ENV{PATH}"
        "LIRIC_TEST_PY_LOG=${default_log}"
        "LIRIC_TEST_CTEST_LOG=${ctest_log}"
        "${BASH_EXE}" "${API_SCRIPT}"
        --workspace "${root}/ws_default"
        --output-root "${root}/out_default"
        --lfortran-dir "${default_lfortran}"
        --skip-checkout
        --skip-lfortran-build
        --run-ref-tests yes
        --run-itests no
        --workers 1
    RESULT_VARIABLE rc_default
    OUTPUT_VARIABLE out_default
    ERROR_VARIABLE err_default
)
if(NOT rc_default EQUAL 0)
    message(FATAL_ERROR
        "default reference policy run failed rc=${rc_default}\nstdout:\n${out_default}\nstderr:\n${err_default}"
    )
endif()
if(NOT EXISTS "${default_log}")
    message(FATAL_ERROR "default reference policy run did not invoke python shim")
endif()
file(READ "${default_log}" default_log_text)
if(NOT default_log_text MATCHES "lfortran_ref_test_liric\\.py")
    message(FATAL_ERROR
        "default reference policy must invoke the liric wrapper script\nlog:\n${default_log_text}"
    )
endif()
if(NOT default_log_text MATCHES "LIRIC_REF_SKIP_IR:[ \t]*[^\n]*llvm")
    message(FATAL_ERROR
        "default reference policy must set LIRIC_REF_SKIP_IR containing llvm\nlog:\n${default_log_text}"
    )
endif()
set(default_summary "${root}/out_default/summary.json")
if(NOT EXISTS "${default_summary}")
    message(FATAL_ERROR "default run did not produce summary.json")
endif()
file(READ "${default_summary}" default_summary_text)
if(NOT default_summary_text MATCHES "\"pass\"[ \t]*:[ \t]*true")
    message(FATAL_ERROR "default run summary must report pass=true\nsummary:\n${default_summary_text}")
endif()

file(REMOVE "${override_log}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${fake_bin}:$ENV{PATH}"
        "LIRIC_TEST_PY_LOG=${override_log}"
        "LIRIC_TEST_CTEST_LOG=${ctest_log}"
        "${BASH_EXE}" "${API_SCRIPT}"
        --workspace "${root}/ws_override"
        --output-root "${root}/out_override"
        --lfortran-dir "${override_lfortran}"
        --skip-checkout
        --skip-lfortran-build
        --run-ref-tests yes
        --run-itests no
        --workers 1
        --ref-args "--backend llvm"
    RESULT_VARIABLE rc_override
    OUTPUT_VARIABLE out_override
    ERROR_VARIABLE err_override
)
if(NOT rc_override EQUAL 0)
    message(FATAL_ERROR
        "override reference policy run failed rc=${rc_override}\nstdout:\n${out_override}\nstderr:\n${err_override}"
    )
endif()
if(NOT EXISTS "${override_log}")
    message(FATAL_ERROR "override reference policy run did not invoke python shim")
endif()
file(READ "${override_log}" override_log_text)
if(NOT override_log_text MATCHES "lfortran_ref_test_liric\\.py")
    message(FATAL_ERROR
        "override run must still invoke the liric wrapper script\nlog:\n${override_log_text}"
    )
endif()
if(NOT override_log_text MATCHES "--backend[ \t]+llvm")
    message(FATAL_ERROR "override run must pass through explicit backend selection\nlog:\n${override_log_text}")
endif()
if(NOT EXISTS "${ctest_log}")
    message(FATAL_ERROR "reference policy runs must invoke ctest for build-liric unit suites")
endif()
file(READ "${ctest_log}" ctest_args_text)
if(NOT ctest_args_text MATCHES "--test-dir[ \t]+${default_lfortran}/build-liric")
    message(FATAL_ERROR
        "default reference policy run must execute ctest against build-liric\nargs:\n${ctest_args_text}"
    )
endif()
if(NOT ctest_args_text MATCHES "--test-dir[ \t]+${override_lfortran}/build-liric")
    message(FATAL_ERROR
        "override reference policy run must execute ctest against build-liric\nargs:\n${ctest_args_text}"
    )
endif()
