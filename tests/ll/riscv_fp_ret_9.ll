define i32 @main() {
entry:
  %sum = fadd double 4.5, 4.5
  %ret = fptosi double %sum to i32
  ret i32 %ret
}
