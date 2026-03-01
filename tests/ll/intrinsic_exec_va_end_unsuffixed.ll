declare void @llvm.va_end(ptr)

define i32 @main() {
entry:
  %ap = alloca i8, align 1
  call void @llvm.va_end(ptr %ap)
  ret i32 77
}
