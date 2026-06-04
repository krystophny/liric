; Dual-PHI loop rotation fixture (issue 512).
; Tests parallel phi copies on conditional branch edges.
; No external function calls -- portable across Linux and macOS.
; Expected result: main() returns 3.

define i32 @main() {
entry:
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %backedge ]
  %acc = phi i32 [ 0, %entry ], [ %acc.upd, %backedge ]
  %i.next = add nuw nsw i32 %i, 1
  %odd = and i32 %i, 1
  %is_even = icmp eq i32 %odd, 0
  br i1 %is_even, label %bump, label %nobump

bump:
  %bumped = add nsw i32 %acc, 1
  br label %merge

nobump:
  br label %merge

merge:
  %acc.upd = phi i32 [ %bumped, %bump ], [ %acc, %nobump ]
  %done = icmp eq i32 %acc.upd, 2
  br i1 %done, label %exit, label %backedge

backedge:
  br label %header

exit:
  ret i32 %i.next
}
