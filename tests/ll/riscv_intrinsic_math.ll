; RISC-V 64 intrinsic math test: fabs, sqrt, copysign, floor, ceil
; Expected exit code: 42

declare double @llvm.fabs.f64(double)
declare double @llvm.sqrt.f64(double)
declare double @llvm.copysign.f64(double, double)
declare double @llvm.floor.f64(double)
declare double @llvm.ceil.f64(double)

define i32 @main() {
entry:
  ; fabs(-9.0) = 9.0
  %a = call double @llvm.fabs.f64(double -9.0)
  %ai = fptosi double %a to i32

  ; sqrt(16.0) = 4.0
  %b = call double @llvm.sqrt.f64(double 16.0)
  %bi = fptosi double %b to i32

  ; copysign(5.0, -1.0) = -5.0, fabs => 5.0
  %c0 = call double @llvm.copysign.f64(double 5.0, double -1.0)
  %c = call double @llvm.fabs.f64(double %c0)
  %ci = fptosi double %c to i32

  ; floor(7.9) = 7.0
  %d = call double @llvm.floor.f64(double 7.9)
  %di = fptosi double %d to i32

  ; ceil(16.1) = 17.0
  %e = call double @llvm.ceil.f64(double 16.1)
  %ei = fptosi double %e to i32

  ; 9 + 4 + 5 + 7 + 17 = 42
  %s1 = add i32 %ai, %bi
  %s2 = add i32 %s1, %ci
  %s3 = add i32 %s2, %di
  %s4 = add i32 %s3, %ei
  ret i32 %s4
}
