; Regression fixture for the LLVM-backend LL text emitter: store / load and a
; pointer-through-pointer store must serialize with opaque `ptr` operands, not
; the legacy `<pointee>*` form (which LLVM's opaque-pointer parser rejects with
; "ptr* is invalid - use ptr instead"). main computes 42 through memory.
define i32 @main() {
entry:
  %x = alloca i32
  store i32 42, ptr %x
  %pp = alloca ptr
  store ptr %x, ptr %pp
  %q = load ptr, ptr %pp
  %v = load i32, ptr %q
  ret i32 %v
}
