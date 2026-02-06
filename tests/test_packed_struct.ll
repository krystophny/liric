; Test that packed struct parsing still works
define i32 @packed_struct_test() {
entry:
  %s = alloca <{ i8, i32 }>, align 4
  ret i32 0
}
