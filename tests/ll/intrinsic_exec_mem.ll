declare void @llvm.memset.p0i8.i32(ptr, i8, i32, i1)
declare void @llvm.memcpy.p0i8.p0i8.i32(ptr, ptr, i32, i1)
declare void @llvm.memmove.p0i8.p0i8.i32(ptr, ptr, i32, i1)

define i32 @main() {
entry:
  %src = alloca [8 x i8], align 1
  %dst = alloca [8 x i8], align 1
  %srcp = getelementptr inbounds [8 x i8], ptr %src, i64 0, i64 0
  %dstp = getelementptr inbounds [8 x i8], ptr %dst, i64 0, i64 0

  call void @llvm.memset.p0i8.i32(ptr %srcp, i8 65, i32 8, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i32(ptr %dstp, ptr %srcp, i32 8, i1 false)
  %dstp1 = getelementptr i8, ptr %dstp, i64 1
  call void @llvm.memmove.p0i8.p0i8.i32(ptr %dstp1, ptr %dstp, i32 6, i1 false)

  %b0 = load i8, ptr %dstp, align 1
  %b1 = load i8, ptr %dstp1, align 1
  %i0 = zext i8 %b0 to i32
  %i1 = zext i8 %b1 to i32
  %sum = add i32 %i0, %i1
  ret i32 %sum
}
