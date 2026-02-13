if(NOT DEFINED SOURCE_DIR OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "SOURCE_DIR and WORKDIR are required")
endif()

find_program(BASH_EXE bash)
if(NOT BASH_EXE)
    message(STATUS "bash not available; skipping llvm backend/compat policy test")
    return()
endif()

find_program(CHMOD_EXE chmod)
if(NOT CHMOD_EXE)
    message(FATAL_ERROR "chmod is required for llvm backend/compat policy test")
endif()

set(root "${WORKDIR}/llvm_backend_compat_policy")
file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${root}")

function(write_fake_llvm_config script_path version_str include_dir)
    set(script_template [=[
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  --version)
    echo "__LIRIC_FAKE_VERSION__"
    ;;
  --includedir)
    echo "__LIRIC_FAKE_INCLUDEDIR__"
    ;;
  --cflags)
    echo ""
    ;;
  --ldflags)
    echo ""
    ;;
  --libs)
    echo ""
    ;;
  --system-libs)
    echo ""
    ;;
  *)
    echo "fake llvm-config: unsupported arg $1" >&2
    exit 91
    ;;
esac
]=])
    string(REPLACE "__LIRIC_FAKE_VERSION__" "${version_str}" script_text "${script_template}")
    string(REPLACE "__LIRIC_FAKE_INCLUDEDIR__" "${include_dir}" script_text "${script_text}")
    file(WRITE "${script_path}" "${script_text}")
    execute_process(COMMAND "${CHMOD_EXE}" +x "${script_path}")
endfunction()

function(run_policy_case case_name version_str with_compat expect_success)
    set(case_root "${root}/${case_name}")
    set(case_build "${case_root}/build")
    set(case_include "${case_root}/include")
    set(fake_llvm_config "${case_root}/llvm-config")

    file(MAKE_DIRECTORY "${case_root}")
    file(MAKE_DIRECTORY "${case_include}")
    write_fake_llvm_config("${fake_llvm_config}" "${version_str}" "${case_include}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${case_build}" -G Ninja
            -DWITH_REAL_LLVM_BACKEND=ON
            -DWITH_LLVM_COMPAT=${with_compat}
            -DWITH_BENCH_TCC=OFF
            -DWITH_BENCH_LLI_PHASES=OFF
            -DLIRIC_LLVM_CONFIG_EXE=${fake_llvm_config}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )

    if(expect_success)
        if(NOT rc EQUAL 0)
            message(FATAL_ERROR
                "configure unexpectedly failed for ${case_name} rc=${rc}\nstdout:\n${out}\nstderr:\n${err}"
            )
        endif()
        return()
    endif()

    if(rc EQUAL 0)
        message(FATAL_ERROR "configure unexpectedly succeeded for ${case_name}")
    endif()

    set(combined "${out}\n${err}")
    if(NOT combined MATCHES
        "Unsupported configuration: WITH_REAL_LLVM_BACKEND=ON and[ \n\r\t]*WITH_LLVM_COMPAT=ON"
    )
        message(FATAL_ERROR
            "configure failed for ${case_name}, but unsupported-combo message missing\nstdout:\n${out}\nstderr:\n${err}"
        )
    endif()
endfunction()

run_policy_case("llvm10_with_compat_on" "10.0.1" "ON" OFF)
run_policy_case("llvm10_with_compat_off" "10.0.1" "OFF" ON)
run_policy_case("llvm11_with_compat_on" "11.1.0" "ON" ON)
