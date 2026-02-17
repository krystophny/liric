define i32 @fib(i32 %n) {
entry:
  %cmp = icmp sle i32 %n, 1
  br i1 %cmp, label %base, label %rec
base:
  ret i32 %n
rec:
  %n1 = sub i32 %n, 1
  %f1 = call i32 @fib(i32 %n1)
  %n2 = sub i32 %n, 2
  %f2 = call i32 @fib(i32 %n2)
  %r = add i32 %f1, %f2
  ret i32 %r
}
define i32 @main() {
entry:
  %r = call i32 @fib(i32 20)
  %rc = srem i32 %r, 256
  ret i32 %rc
}
