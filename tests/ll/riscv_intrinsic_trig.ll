; RISC-V 64 intrinsic transcendental test: sin, cos, exp
; Expected exit code: 42
;
; Limited to 7 call results to stay within the 14-GPR temp pool
; (7 fptosi + 6 add = 13 GPR temporaries).

declare double @llvm.sin.f64(double)
declare double @llvm.cos.f64(double)
declare double @llvm.exp.f64(double)

define i32 @main() {
entry:
  ; cos(0.0) = 1.0 -> 1
  %a = call double @llvm.cos.f64(double 0.0)
  %ai = fptosi double %a to i32

  ; exp(3.0) = 20.085... -> 20
  %b = call double @llvm.exp.f64(double 3.0)
  %bi = fptosi double %b to i32

  ; exp(2.0) = 7.389... -> 7
  %c = call double @llvm.exp.f64(double 2.0)
  %ci = fptosi double %c to i32

  ; exp(2.2) = 9.025... -> 9
  %d = call double @llvm.exp.f64(double 2.2)
  %di = fptosi double %d to i32

  ; exp(1.5) = 4.481... -> 4
  %e = call double @llvm.exp.f64(double 1.5)
  %ei = fptosi double %e to i32

  ; sin(0.0) = 0.0 -> 0
  %f = call double @llvm.sin.f64(double 0.0)
  %fi = fptosi double %f to i32

  ; cos(0.0) = 1.0 -> 1
  %g = call double @llvm.cos.f64(double 0.0)
  %gi = fptosi double %g to i32

  ; 1 + 20 + 7 + 9 + 4 + 0 + 1 = 42
  %s1 = add i32 %ai, %bi
  %s2 = add i32 %s1, %ci
  %s3 = add i32 %s2, %di
  %s4 = add i32 %s3, %ei
  %s5 = add i32 %s4, %fi
  %s6 = add i32 %s5, %gi
  ret i32 %s6
}
