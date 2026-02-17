define i32 @sum_to(i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [1, %entry], [%i_next, %loop]
  %s = phi i32 [0, %entry], [%s_next, %loop]
  %s_next = add i32 %s, %i
  %i_next = add i32 %i, 1
  %cmp = icmp sle i32 %i_next, %n
  br i1 %cmp, label %loop, label %done
done:
  ret i32 %s_next
}
define i32 @main() {
entry:
  %r = call i32 @sum_to(i32 10)
  ret i32 %r
}
