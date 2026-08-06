# LIRIC roadmap

Snapshot: 2026-08-06. LIRIC owns the backend-neutral public session ABI,
verification, serialization, and object emission used by ffc. Fortran
semantics, descriptors, and lowering policy remain in FortFront/ffc.

## Current truth

The audited baseline is `3facb898`. Producer/compatibility/nightly evidence
is not green: compatibility [run 31074525402](https://github.com/krystophny/liric/actions/runs/31074525402)
is red for missing `llvm-dwarfdump` on Linux and an incompatible macOS bison;
nightly [run 31076199691](https://github.com/krystophny/liric/actions/runs/31076199691)
fails while building LFortran (`LIBUNWIND`) and has missing result artifacts;
the bench matrix [run 30805088672](https://github.com/krystophny/liric/actions/runs/30805088672)
also lacks the `liric/liric_session.h` producer artifact. These failures are
tracked by [#533](https://github.com/krystophny/liric/issues/533), which gates
[#523](https://github.com/krystophny/liric/issues/523): they are not evidence
for or against the serializer defect and must not be reported as semantic
passes.

## Immediate order

1. Restore producer-build artifacts and the scheduled platform/tool
   compatibility jobs under [#533](https://github.com/krystophny/liric/issues/533),
   with explicit tool discovery, minimum versions, and a skip/fail policy
   that names the missing tool or incompatible version.
2. Reduce #523 to a public C session producer. Verify the IR before and after
   serialization, emit an object, link a small consumer, and compare behavior.
   This assigns ownership between ffc's producer and LIRIC's serializer without
   relying on ffc internals.
3. Fix the owning layer, then run the same public producer through current ffc
   generated-code/runtime gates.
4. Freeze and version the session/serialized schema needed by ffc module and
   runtime ABI work. Reject unknown major versions and target-incompatible
   artifacts explicitly.

## Contract

- Session operations have explicit types, ownership, lifetime, error results,
  and verification points. No hidden process-global state controls emission.
- Serialization round-trips definitions, CFG/dominance, types, symbols,
  target information, and required runtime declarations without depending on
  insertion order.
- Public headers and Fortran bindings are generated or checked from one API
  definition. ABI changes update both in one commit.
- Verification precedes object emission. A verifier failure cannot produce or
  cache a reusable artifact.
- Performance counters separate session construction, verification,
  serialization, optimization, and object emission, with peak RSS and output
  size.

## Delivery gates

Every ABI or serializer change needs:

- a minimal public C-session behavioral reproducer and a deliberately invalid
  negative case;
- before/after serialization structural equivalence plus object
  compile/link/run output;
- current public-header and Fortran-binding compatibility tests;
- the affected ffc generated-code/runtime cluster at pinned revisions; and
- one full `fo` pipeline before the final commit.

For performance claims, validate generated behavior first, interleave old/new
runs on a controlled host, repeat, and report effect size and confidence
interval. Update ffc's runtime/ABI documentation in the same linked changes
when the public contract changes.
