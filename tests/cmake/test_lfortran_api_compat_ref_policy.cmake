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

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${fake_bin}")

set(fake_python "${fake_bin}/python")
file(WRITE "${fake_python}" "#!/usr/bin/env bash
set -euo pipefail
if [[ -n \"\${LIRIC_TEST_PY_LOG:-}\" ]]; then
    printf '%s\\n' \"\$*\" >> \"\${LIRIC_TEST_PY_LOG}\"
fi
exit 0
")
execute_process(COMMAND "${CHMOD_EXE}" +x "${fake_python}")

function(make_fake_lfortran_tree path)
    file(MAKE_DIRECTORY "${path}/.git")
    file(MAKE_DIRECTORY "${path}/build-liric/src/bin")
    file(MAKE_DIRECTORY "${path}/integration_tests")
    file(WRITE "${path}/build-liric/src/bin/lfortran" "#!/usr/bin/env bash
exit 0
")
    file(WRITE "${path}/run_tests.py" "print('stub')\n")
    file(WRITE "${path}/integration_tests/run_tests.py" "print('stub')\n")
    execute_process(COMMAND "${CHMOD_EXE}" +x "${path}/build-liric/src/bin/lfortran")
endfunction()

make_fake_lfortran_tree("${default_lfortran}")
make_fake_lfortran_tree("${override_lfortran}")

file(REMOVE "${default_log}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${fake_bin}:$ENV{PATH}"
        "LIRIC_TEST_PY_LOG=${default_log}"
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
file(READ "${default_log}" default_args_text)
if(NOT default_args_text MATCHES "--exclude-backend[ \t]+llvm")
    message(FATAL_ERROR
        "default reference policy must suppress llvm backend snapshots\nargs:\n${default_args_text}"
    )
endif()

file(REMOVE "${override_log}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${fake_bin}:$ENV{PATH}"
        "LIRIC_TEST_PY_LOG=${override_log}"
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
file(READ "${override_log}" override_args_text)
if(NOT override_args_text MATCHES "--backend[ \t]+llvm")
    message(FATAL_ERROR "override run must keep explicit backend selection\nargs:\n${override_args_text}")
endif()
if(override_args_text MATCHES "--exclude-backend[ \t]+llvm")
    message(FATAL_ERROR
        "override run must not inject default llvm exclusion when backend policy is explicit\nargs:\n${override_args_text}"
    )
endif()
