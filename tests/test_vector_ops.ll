; Test vector type with operations
define i32 @vector_load_store_test() {
entry:
  %vec = alloca <2 x float>, align 8
  %ptr = bitcast <2 x float>* %vec to i8*
  store i8 42, i8* %ptr, align 1
  %val = load i8, i8* %ptr, align 1
  %result = zext i8 %val to i32
  ret i32 %result
}
