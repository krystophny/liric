define i32 @main() {
entry:
  br label %loop
loop:
  %i = phi i32 [0, %entry], [%next, %loop]
  %sum = phi i32 [0, %entry], [%sum_next, %loop]
  %next = add i32 %i, 1
  %sum_next = add i32 %sum, %next
  %done = icmp eq i32 %next, 10
  br i1 %done, label %exit, label %loop
exit:
  ret i32 %sum_next
}
