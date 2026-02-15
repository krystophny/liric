if(NOT DEFINED BENCH_COMPAT_CHECK OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "BENCH_COMPAT_CHECK and WORKDIR are required")
endif()

set(root "${WORKDIR}/compat_check_freeze")
set(bench_dir "${root}/bench")
set(int_dir "${root}/integration_tests")
set(fake_lfortran "${root}/fake_lfortran.sh")
set(fake_probe "${root}/fake_probe_runner.sh")
set(fake_lli "${root}/fake_lli.sh")
set(runtime_lib "${root}/libfake_runtime.so")

file(REMOVE_RECURSE "${root}")
file(MAKE_DIRECTORY "${bench_dir}")
file(MAKE_DIRECTORY "${int_dir}")

file(WRITE "${int_dir}/CMakeLists.txt"
"RUN(NAME fake_api FILE fake_api.f90 LABELS llvm)\n")
file(WRITE "${int_dir}/fake_api.f90"
"program fake_api\n"
"print *, 42\n"
"end program\n")

file(WRITE "${runtime_lib}" "fake")

file(WRITE "${fake_lfortran}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"show_llvm=0\n"
"out=\"\"\n"
"for arg in \"$@\"; do\n"
"  if [[ \"$arg\" == \"--show-llvm\" ]]; then\n"
"    show_llvm=1\n"
"  fi\n"
"done\n"
"if [[ \"$show_llvm\" -eq 1 ]]; then\n"
"  cat <<'IR'\n"
"define i32 @main(i32 %argc, i8** %argv) {\n"
"entry:\n"
"  ret i32 0\n"
"}\n"
"IR\n"
"  exit 0\n"
"fi\n"
"while [[ $# -gt 0 ]]; do\n"
"  if [[ \"$1\" == \"-o\" ]]; then\n"
"    out=\"$2\"\n"
"    shift 2\n"
"  else\n"
"    shift\n"
"  fi\n"
"done\n"
"if [[ -z \"$out\" ]]; then\n"
"  exit 1\n"
"fi\n"
"cat > \"$out\" <<'BIN'\n"
"#!/usr/bin/env bash\n"
"echo 42\n"
"exit 0\n"
"BIN\n"
"chmod +x \"$out\"\n"
"exit 0\n")

file(WRITE "${fake_probe}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"echo 42\n"
"exit 0\n")

file(WRITE "${fake_lli}" "#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"echo 42\n"
"exit 0\n")

execute_process(COMMAND chmod +x "${fake_lfortran}" "${fake_probe}" "${fake_lli}"
                RESULT_VARIABLE chmod_rc)
if(NOT chmod_rc EQUAL 0)
    message(FATAL_ERROR "chmod failed")
endif()

execute_process(
    COMMAND "${BENCH_COMPAT_CHECK}"
        --timeout 5
        --limit 1
        --lfortran "${fake_lfortran}"
        --probe-runner "${fake_probe}"
        --runtime-lib "${runtime_lib}"
        --lli "${fake_lli}"
        --cmake "${int_dir}/CMakeLists.txt"
        --bench-dir "${bench_dir}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bench_compat_check failed rc=${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()

set(compat_api "${bench_dir}/compat_api.txt")
set(compat_api_100 "${bench_dir}/compat_api_100.txt")
set(compat_api_100_opts "${bench_dir}/compat_api_100_options.jsonl")

if(NOT EXISTS "${compat_api}")
    message(FATAL_ERROR "missing compat_api artifact: ${compat_api}")
endif()
if(NOT EXISTS "${compat_api_100}")
    message(FATAL_ERROR "missing frozen artifact: ${compat_api_100}")
endif()
if(NOT EXISTS "${compat_api_100_opts}")
    message(FATAL_ERROR "missing frozen options artifact: ${compat_api_100_opts}")
endif()

file(READ "${compat_api}" compat_api_text)
if(NOT compat_api_text MATCHES "fake_api")
    message(FATAL_ERROR "compat_api.txt did not include fake_api:\n${compat_api_text}")
endif()

file(READ "${compat_api_100}" compat_api_100_text)
if(NOT compat_api_100_text MATCHES "fake_api")
    message(FATAL_ERROR "compat_api_100.txt did not include fake_api:\n${compat_api_100_text}")
endif()

file(READ "${compat_api_100_opts}" compat_api_100_opts_text)
if(NOT compat_api_100_opts_text MATCHES "\"name\":\"fake_api\"")
    message(FATAL_ERROR "compat_api_100_options.jsonl missing fake_api row:\n${compat_api_100_opts_text}")
endif()
if(NOT compat_api_100_opts_text MATCHES "\"source\":\"fake_api\\.f90\"")
    message(FATAL_ERROR "compat_api_100_options.jsonl missing fake_api source row:\n${compat_api_100_opts_text}")
endif()
