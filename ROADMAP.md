# LIRIC roadmap

LIRIC owns the backend-neutral public session ABI, verification, and object
emission used by ffc. Fortran-language semantics stay in FortFront/ffc; ABI
changes require generated-code and runtime evidence.

## Current handoff (2026-08-03)

- The backend implementation baseline is `5436e5c`; the roadmap commits are
  pushed on current `main`.
- PR #524 is merged and the session/header CI contract from #528 is closed.
- The remaining open backend blocker is
  [#523](https://github.com/krystophny/liric/issues/523): serialized
  dominance must preserve integer print definitions.

## Downstream contract

ffc [#375](https://github.com/lazy-fortran/ffc/issues/375) consumes the public
ABI. Array-section work in ffc [#337](https://github.com/lazy-fortran/ffc/issues/337)
must keep descriptor views, generated addresses, and print/runtime definitions
valid across serialization. Keep `LIBRARY_PATH` and the exact LIRIC revision
explicit in every ffc verification run.

## Delivery gate

Every ABI change needs a public-session regression, a generated-code or object
oracle, compatibility checks, and the full `fo` pipeline. Update downstream ffc
roadmap links when an ABI issue opens, closes, or changes its contract.
