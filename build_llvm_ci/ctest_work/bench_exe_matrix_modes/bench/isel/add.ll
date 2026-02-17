define i32 @add(i32 %a, i32 %b) {
entry:
  %c = add i32 %a, %b
  ret i32 %c
}
define i32 @main() {
entry:
  %r = call i32 @add(i32 10, i32 32)
  ret i32 %r
}
