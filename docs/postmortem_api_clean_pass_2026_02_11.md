# Postmortem: API Clean-Pass Claim Failed (2026-02-11)

## Summary
On 2026-02-11, we expected recent issue closures to imply API-mode clean pass.
Fresh benchmark evidence contradicted that expectation.

Fresh run (`d86bb64`, 2026-02-11 08:13 +0100):
- `attempted=2125`
- `completed=388`
- `skipped=1737`
- `failed=1737`

Primary skip buckets:
- `liric_jit_sigsegv=1258`
- `liric_jit_failed=357`
- `liric_jit_timeout=92`
- `liric_jit_sigabrt=18`

## Impact
- We nearly accepted completion despite large remaining failure volume.
- Work was correctly improving slices, but not enforcing end-to-end completion criteria.
- Confidence in status reporting was reduced.

## What Happened
1. Several focused issues were closed after local improvements.
2. A fresh full-corpus API benchmark was not used as mandatory closure evidence.
3. Default benchmark invocation permits skips unless explicitly gated.
4. A stale compat entry (`realloc_lhs_17`) was present in `/tmp` artifacts, showing artifact hygiene risk.

## Root Causes
1. Closure criteria were bucket-local, not global.
2. No single mandatory command enforced API clean-pass before declaring success.
3. Evidence requirements for issue closure were inconsistent.
4. Artifact freshness relied on convention rather than a strict final gate.

## Contributing Factors
- Large corpus size and mixed failure modes made progress appear larger than it was.
- Existing gate behavior (`--require-zero-skips`) was available but not mandatory.
- Temporary `/tmp` artifacts can drift across runs.

## Corrective Actions (Implemented)
1. Added hard gate script: `tools/bench_api_clean_gate.sh`
   - Runs `bench_compat_check` and `bench_api --require-zero-skips`
   - Fails unless:
     - `skipped == 0`
     - `attempted == completed`
     - `failed == 0`
     - `zero_skip_gate_met == true`
2. Updated docs to make the gate command required before API clean-pass claims:
   - `README.md`
   - `docs/lfortran_mass_testing.md`
3. Opened enforced chunked issue program with explicit close criteria:
   - #253, #254, #255, #256, #257
   - Meta tracker: #258

## Preventive Policy (Going Forward)
- Do not close API clean-pass issues without gate output from `tools/bench_api_clean_gate.sh`.
- Keep chunk issues open until their specific bucket is zero in fresh summary artifacts.
- Keep meta issue open until full zero-skip gate passes.

## Verification Command
```bash
./tools/bench_api_clean_gate.sh
```

