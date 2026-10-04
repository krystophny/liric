# LFortran compatibility goals

Make the supported LIRIC backend a correct drop-in consumer path with minimal
required upstream deltas. Both LLVM IR replay and compile-time API use must
produce independently correct results under their declared scope.

## Success

- Current hard crashes, unsupported ABI failures and semantic/output mismatches
  are reproduced with exact producer/backend identities and assigned to their
  actual owner.
- Backend defects are fixed in LIRIC; genuine producer defects receive the
  smallest upstream-compatible repair rather than a permanent backend workaround.
- Focused regressions and the original affected consumer establish each repair.
- A declared drop-in milestone has complete relevant replay/API evidence and
  the promised absence of LLVM runtime dependencies in the WITH_LIRIC binary.
- Missing producer tools/artifacts, unsupported cases and incomplete audit work
  are explicit, never counted as parity.

The [case evidence](failure_task_list.md) and
[taxonomy](../lfortran_failure_taxonomy.md) record observations. Internal
investigation order and debugging technique are agent choices. Apply
[development principles](https://github.com/lazy-fortran/fo/blob/main/doc/GOAL_DRIVEN_DEVELOPMENT.md)
and [ROADMAP goals](../../ROADMAP.md); independent audit CI does not block useful
local implementation. Historical recipes remain in
[the earlier plan](https://github.com/krystophny/liric/blob/5bb0e02ebac0faf77249eabc492644bc525099cb/docs/lfortran_mass/investigation_plan.md).
