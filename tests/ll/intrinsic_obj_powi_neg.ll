declare double @llvm.powi.f64.i32(double, i32)

define i32 @main() {
entry:
  %pow = call double @llvm.powi.f64.i32(double 2.000000e+00, i32 -1)
  %scaled = fmul double %pow, 2.000000e+00
  %ival = fptosi double %scaled to i32
  %ok = icmp eq i32 %ival, 1
  %ret = select i1 %ok, i32 0, i32 97
  ret i32 %ret
}
