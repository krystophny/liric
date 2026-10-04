@tls = thread_local global i64 17, align 64
@observed = global [4 x i64] zeroinitializer

declare void @GOMP_parallel(ptr, ptr, i32, i32)
declare void @GOMP_barrier()
declare void @omp_set_dynamic(i32)
declare i32 @omp_get_thread_num()
declare void @abort()

define void @worker(ptr %unused) {
entry:
  %tid = call i32 @omp_get_thread_num()
  %id = sext i32 %tid to i64
  %initial = load i64, ptr @tls
  %ok = icmp eq i64 %initial, 17
  br i1 %ok, label %ready, label %fail
ready:
  call void @GOMP_barrier()
  br label %loop
loop:
  %turn = phi i32 [0, %ready], [%next, %after]
  %mine = icmp eq i32 %tid, %turn
  br i1 %mine, label %write, label %wait
write:
  %value = add i64 %id, 100
  store i64 %value, ptr @tls
  br label %wait
wait:
  call void @GOMP_barrier()
  br label %after
after:
  %next = add i32 %turn, 1
  %done = icmp eq i32 %next, 4
  br i1 %done, label %check, label %loop
check:
  %actual = load i64, ptr @tls
  %expected = add i64 %id, 100
  %private = icmp eq i64 %actual, %expected
  br i1 %private, label %record, label %fail
record:
  %slot = getelementptr [4 x i64], ptr @observed, i64 0, i64 %id
  store i64 %actual, ptr %slot
  ret void
fail:
  call void @abort()
  unreachable
}

define i32 @main() {
entry:
  call void @omp_set_dynamic(i32 0)
  call void @GOMP_parallel(ptr @worker, ptr null, i32 4, i32 0)
  br label %loop
loop:
  %id = phi i64 [0, %entry], [%next, %advance]
  %slot = getelementptr [4 x i64], ptr @observed, i64 0, i64 %id
  %actual = load i64, ptr %slot
  %expected = add i64 %id, 100
  %ok = icmp eq i64 %actual, %expected
  br i1 %ok, label %advance, label %fail
advance:
  %next = add i64 %id, 1
  %done = icmp eq i64 %next, 4
  br i1 %done, label %check, label %loop
check:
  %main = load i64, ptr @tls
  %preserved = icmp eq i64 %main, 100
  br i1 %preserved, label %success, label %fail
success:
  ret i32 0
fail:
  ret i32 1
}
