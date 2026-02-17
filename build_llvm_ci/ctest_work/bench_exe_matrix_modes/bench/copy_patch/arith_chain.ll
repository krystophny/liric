define i32 @arith(i32 %a, i32 %b) {
entry:
  %sum = add i32 %a, %b
  %prod = mul i32 %sum, %b
  %diff = sub i32 %prod, %a
  ret i32 %diff
}
define i32 @main() {
entry:
  %r = call i32 @arith(i32 3, i32 4)
  ret i32 %r
}
