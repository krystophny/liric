@counter = thread_local global i64 17, align 64
@zero = thread_local(localexec) global [32 x i8] zeroinitializer, align 128
@bytes = thread_local(initialexec) global [4 x i8] c"ABCD", align 16
define ptr @tls_counter() {
entry:
  ret ptr @counter
}
define ptr @tls_zero() {
entry:
  ret ptr @zero
}
define ptr @tls_bytes() {
entry:
  ret ptr @bytes
}
define ptr @tls_offset() {
entry:
  %p = getelementptr i8, ptr @bytes, i64 2
  ret ptr %p
}
