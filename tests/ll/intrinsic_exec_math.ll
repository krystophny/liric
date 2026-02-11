declare float @llvm.fabs.f32(float)
declare float @llvm.sqrt.f32(float)
declare float @llvm.copysign.f32(float, float)
declare float @llvm.powi.f32.i32(float, i32)
declare double @llvm.exp.f64(double)
declare double @llvm.pow.f64(double, double)

define i32 @main() {
entry:
  %a = call float @llvm.fabs.f32(float -3.5)
  %ai = fptosi float %a to i32

  %b = call float @llvm.sqrt.f32(float 9.0)
  %bi = fptosi float %b to i32

  %c = call float @llvm.copysign.f32(float 3.0, float -1.0)
  %ci = fptosi float %c to i32

  %d = call float @llvm.powi.f32.i32(float 2.0, i32 3)
  %di = fptosi float %d to i32

  %e = call double @llvm.exp.f64(double 0.0)
  %ei = fptosi double %e to i32

  %f = call double @llvm.pow.f64(double 2.0, double 3.0)
  %fi = fptosi double %f to i32

  %s0 = add i32 %ai, %bi
  %s1 = add i32 %s0, %ci
  %s2 = add i32 %s1, %di
  %s3 = add i32 %s2, %ei
  %s4 = add i32 %s3, %fi
  ret i32 %s4
}
