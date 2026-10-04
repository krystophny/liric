# Direct-mode compatibility goals

Maintain the promised LLVM 21 compatibility and supported direct/no-link
execution behavior. Valid optimized IR must execute correctly without hidden
semantic fallback, consumer-specific special cases or diagnostic side effects.
Choose the smallest adequate repair under the [development principles](https://github.com/lazy-fortran/fo/blob/main/doc/GOAL_DRIVEN_DEVELOPMENT.md).

## Observable success

- Baseline and optimized formatter cases produce independently correct output
  and terminate under the documented supported lane.
- Integer widths, signed/unsigned operations, control flow and pointer/index
  behavior preserve the applicable LLVM semantics.
- The actual LFortran API consumer retains compatible output and runtime/ABI
  behavior; a backend defect is repaired here and rechecked there.
- Meaningful focused regression coverage remains, with obsolete debug wrappers
  and unnecessary maintained code removed.

## Historical reproductions

The 2026-02-25 checkpoint at `5f21f48` observed a working `call_sff_link.bc` and
formatter mismatches/hangs in `call_sff_O1_link.bc`, `call_sff_O1O1_link.bc` and
`format_26`/`format_34` integrations. Recheck those current signatures before
assigning a repair; neither old local edits nor suspected opcode causes are a
mandatory implementation recipe.

## Evidence

Use a reduced independent IR/runtime example plus the original consumer path.
Full compatibility campaigns justify their declared completion claims; optional
benchmarks do not gate every repair. Push focused locally verified increments.
[ROADMAP.md](ROADMAP.md) owns active provider goals; historical commands/details
remain in [the earlier note](https://github.com/krystophny/liric/blob/5bb0e02ebac0faf77249eabc492644bc525099cb/TODO.md).
