define i32 @main() {
entry:
  %cmp = icmp sgt i32 5, 3
  br i1 %cmp, label %then, label %else
then:
  ret i32 1
else:
  ret i32 0
}
