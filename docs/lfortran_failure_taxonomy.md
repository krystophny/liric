# LFortran Mass Failure Taxonomy

This document defines the stable root-cause taxonomy used by
`tools/lfortran_mass/report.py` and `tools/lfortran_mass/nightly_mass.sh`
summary artifacts.

## Taxonomy Dimensions

Each non-pass result is mapped to one node:

`<stage>|<symptom>|<feature_family>`

### Stage
- `parse`: LLVM IR parse/IR construction failures.
- `codegen`: LFortran emit failures before liric parse/JIT.
- `jit-link`: entrypoint/signature/symbol resolution failures.
- `runtime`: JIT execution failures after symbol/link setup.
- `output-format`: differential run completed but outputs differ.

### Symptom
- `segfault`
- `unresolved-symbol`
- `wrong-stdout`
- `wrong-stderr`
- `wrong-stdout+stderr`
- `rc-mismatch`
- `unsupported-feature`
- `unsupported-abi`
- `compiler-error`
- `infra-fail`
- `unknown`

### Feature Family
- `intrinsics`
- `complex`
- `openmp`
- `multi-file`
- `runtime-api`
- `general`

## Bucket Mapping For Umbrella Issues

This maps historical bucket issues to concrete child issues.
Machine-readable ownership for shell workflow artifacts is kept in
`tools/lfortran_mass/unsupported_bucket_map.json`.

### Differential mismatch bucket (`#80`, closed)

- `output-format|wrong-stdout|general` -> `#54`
- `output-format|wrong-stderr|general` -> `#55`
- `output-format|rc-mismatch|general` -> `#16`
- `output-format|wrong-stdout+stderr|general` -> `#16`

### Unsupported bucket (`#81`, open)

- `jit-link|unresolved-symbol|runtime-api` -> `#79`
- `runtime|segfault|general` -> `#78`
- `runtime|unsupported-feature|intrinsics` -> `#76`, `#75`
- `runtime|unsupported-feature|complex` -> `#77`
- `runtime|unsupported-feature|openmp` -> `#82`
- `parse|unsupported-feature|general` -> `#50`, `#51`, `#53`, `#74`

## Reporting

`summary.json` and `summary.md` now include:
- `taxonomy_counts`: all non-pass nodes.
- `mismatch_taxonomy_counts`: only `mismatch` rows (`#80` lineage).
- `unsupported_taxonomy_counts`: only `unsupported_feature`/`unsupported_abi` rows (`#81` lineage).
- `unsupported_bucket_issue_coverage`: unsupported taxonomy buckets annotated with
  mapped issues or deferred rationale.
- `unsupported_bucket_unmapped`: unsupported taxonomy nodes with no mapping/rationale.
