; Regression fixture for issue 522: a duplicate runtime declaration must not
; make the LLVM backend reject the module ("invalid redefinition of function").
; liric dedups redundant declarations so the emitted IR declares each symbol
; at most once. main returns 42.
declare i32 @snprintf(ptr, i64, ptr, ...)
declare i32 @snprintf(ptr, i64, ptr, ...)

define i32 @main() {
entry:
  ret i32 42
}
