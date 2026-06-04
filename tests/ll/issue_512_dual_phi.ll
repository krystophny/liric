; ModuleID = '/tmp/liric512cand7ir.owxCng/test.c'
source_filename = "/tmp/liric512cand7ir.owxCng/test.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

@.str = private unnamed_addr constant [4 x i8] c"2i5\00", align 1
@.str.1 = private unnamed_addr constant [6 x i8] c"x=%d\0A\00", align 1

define dso_local i32 @main() local_unnamed_addr #0 {
  br label %6

1:                                                ; preds = %56
  %2 = add nsw i32 %57, 1
  %3 = icmp slt i32 %57, 2
  %4 = icmp samesign ult i32 %7, 3
  %5 = select i1 %3, i1 %4, i1 false
  br i1 %5, label %6, label %59, !llvm.loop !9

6:                                                ; preds = %0, %1
  %7 = phi i32 [ 0, %0 ], [ %9, %1 ]
  %8 = phi i32 [ 0, %0 ], [ %2, %1 ]
  %9 = add nuw nsw i32 %7, 1
  %10 = call i32 (ptr, ...) @printf(ptr noundef nonnull dereferenceable(1) @.str.1, i32 noundef %8)
  %11 = call ptr @__ctype_b_loc() #3
  %12 = load ptr, ptr %11, align 8, !tbaa !12
  %13 = sext i32 %8 to i64
  %14 = getelementptr inbounds i8, ptr @.str, i64 %13
  %15 = load i8, ptr %14, align 1, !tbaa !15
  %16 = sext i8 %15 to i64
  %17 = getelementptr inbounds i16, ptr %12, i64 %16
  %18 = load i16, ptr %17, align 2, !tbaa !16
  %19 = and i16 %18, 2048
  %20 = icmp eq i16 %19, 0
  br i1 %20, label %56, label %21

21:                                               ; preds = %6
  %22 = sext i32 %8 to i64
  br label %23

23:                                               ; preds = %21, %23
  %24 = phi i64 [ %22, %21 ], [ %32, %23 ]
  %25 = getelementptr inbounds i8, ptr @.str, i64 %24
  %26 = load i8, ptr %25, align 1, !tbaa !15
  %27 = sext i8 %26 to i64
  %28 = getelementptr inbounds i16, ptr %12, i64 %27
  %29 = load i16, ptr %28, align 2, !tbaa !16
  %30 = and i16 %29, 2048
  %31 = icmp eq i16 %30, 0
  %32 = add nsw i64 %24, 1
  br i1 %31, label %33, label %23, !llvm.loop !18

33:                                               ; preds = %23, %33
  %34 = phi i64 [ %42, %33 ], [ %24, %23 ]
  %35 = getelementptr inbounds i8, ptr @.str, i64 %34
  %36 = load i8, ptr %35, align 1, !tbaa !15
  %37 = sext i8 %36 to i64
  %38 = getelementptr inbounds i16, ptr %12, i64 %37
  %39 = load i16, ptr %38, align 2, !tbaa !16
  %40 = and i16 %39, 1024
  %41 = icmp eq i16 %40, 0
  %42 = add nsw i64 %34, 1
  br i1 %41, label %43, label %33, !llvm.loop !19

43:                                               ; preds = %33, %43
  %44 = phi i64 [ %52, %43 ], [ %34, %33 ]
  %45 = getelementptr inbounds i8, ptr @.str, i64 %44
  %46 = load i8, ptr %45, align 1, !tbaa !15
  %47 = sext i8 %46 to i64
  %48 = getelementptr inbounds i16, ptr %12, i64 %47
  %49 = load i16, ptr %48, align 2, !tbaa !16
  %50 = and i16 %49, 2048
  %51 = icmp eq i16 %50, 0
  %52 = add nsw i64 %44, 1
  br i1 %51, label %53, label %43, !llvm.loop !20

53:                                               ; preds = %43
  %54 = trunc nsw i64 %44 to i32
  %55 = add nsw i32 %54, -1
  br label %56

56:                                               ; preds = %53, %6
  %57 = phi i32 [ %55, %53 ], [ %8, %6 ]
  %58 = icmp eq i32 %57, 2
  br i1 %58, label %59, label %1

59:                                               ; preds = %56, %1
  %60 = phi i32 [ 2, %56 ], [ %2, %1 ]
  %61 = add nsw i32 %9, %60
  ret i32 %61
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr noundef readonly captures(none), ...) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare ptr @__ctype_b_loc() local_unnamed_addr #2

attributes #0 = { nofree nounwind sspstrong uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree nounwind "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { nounwind willreturn memory(none) }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}
!llvm.errno.tbaa = !{!5}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 22.1.6"}
!5 = !{!6, !6, i64 0}
!6 = !{!"int", !7, i64 0}
!7 = !{!"omnipotent char", !8, i64 0}
!8 = !{!"Simple C/C++ TBAA"}
!9 = distinct !{!9, !10, !11}
!10 = !{!"llvm.loop.mustprogress"}
!11 = !{!"llvm.loop.unroll.disable"}
!12 = !{!13, !13, i64 0}
!13 = !{!"p1 short", !14, i64 0}
!14 = !{!"any pointer", !7, i64 0}
!15 = !{!7, !7, i64 0}
!16 = !{!17, !17, i64 0}
!17 = !{!"short", !7, i64 0}
!18 = distinct !{!18, !10, !11}
!19 = distinct !{!19, !10, !11}
!20 = distinct !{!20, !10, !11}
