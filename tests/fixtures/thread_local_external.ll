@counter = external thread_local global i64, align 64
define ptr @tls_alias() {
entry:
  ret ptr @counter
}
