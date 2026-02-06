; Test vector type parsing
define i32 @vector_alloca_test() {
entry:
  %complex_ret_tmp = alloca <2 x float>, align 8
  %vec4 = alloca <4 x i32>, align 16
  %vec2d = alloca <2 x double>, align 16
  ret i32 0
}
