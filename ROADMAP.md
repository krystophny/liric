# LIRIC goals

Provide correct public session/backend behavior and emitted artifacts for FFC
and supported LLVM/WebAssembly consumers. Existing public API/ABI and language
semantics remain authoritative. Apply
[goals and architectural freedom](https://github.com/lazy-fortran/fo/blob/main/doc/GOAL_DRIVEN_DEVELOPMENT.md):
defer internal architecture until needed and change it when evidence warrants.

## Current goals

- [#523](https://github.com/krystophny/liric/issues/523): valid control-flow and
  integer output survive public-session construction, serialization and emission.
- [#533](https://github.com/krystophny/liric/issues/533): independent producer
  builds supply usable artifacts; missing setup cannot masquerade as parity.
- [#535](https://github.com/krystophny/liric/issues/535): dependency debug
  information follows the selected downstream profile.
- [Fo #205](https://github.com/lazy-fortran/fo/issues/205): reduce maintained
  backend/test/documentation burden without shifting complexity between providers.

## Required behavior

Public operations expose sufficient types, lifetimes, ownership and errors for
correct consumers. Valid IR retains definitions, control flow, symbols/types and
target semantics through supported serialization/emission. Invalid IR is
diagnosed and cannot produce reusable success. Public installed artifacts are
actually consumable, not merely present in a checkout.

Independent public producers and linked/executed consumers establish behavior.
Repair defects in the owning layer and recheck FFC; do not mask invalid frontend
input with backend special cases. Keep Fortran semantic policy in its owner.

## Delivery and evidence

[FFC PLAN](https://github.com/lazy-fortran/ffc/blob/main/PLAN.md) orders the
compiler program. Actual consumer dependencies determine when a task can run;
unrelated Fo cleanup and independent audit CI are not prerequisites. Use focused
local evidence and publish verified increments promptly.

The installed-header public-consumer repair has prior focused evidence.
Historical producer/tool setup failures do not prove or disprove a serializer
defect. Reproduce current behavior before attributing ownership or claiming green.
Compatibility/nightly/benchmark campaigns report exact identities and unsupported/
unrun work truthfully; performance audits stay advisory during development.

[Direct-mode goals](TODO.md) and
[LFortran compatibility goals](docs/lfortran_mass/investigation_plan.md) describe
scoped behavior rather than compulsory debugging algorithms. Earlier detailed
observations remain at
[the pre-revision roadmap](https://github.com/krystophny/liric/blob/5bb0e02ebac0faf77249eabc492644bc525099cb/ROADMAP.md).
