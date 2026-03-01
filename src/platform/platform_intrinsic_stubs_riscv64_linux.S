.text

# RISC-V 64 intrinsic blobs for liric.
# RV64GC calling convention: GP args a0-a7, FP args fa0-fa7, return a0/fa0.
# Callee-saved: s0-s11, fs0-fs11, ra, sp.
# Scratch: t0-t6, ft0-ft11, a0-a7, fa0-fa7.

.globl lr_stub_llvm_fabs_f32_begin
.globl lr_stub_llvm_fabs_f32_end
lr_stub_llvm_fabs_f32_begin:
    fabs.s fa0, fa0
    ret
lr_stub_llvm_fabs_f32_end:

.globl lr_stub_llvm_fabs_f64_begin
.globl lr_stub_llvm_fabs_f64_end
lr_stub_llvm_fabs_f64_begin:
    fabs.d fa0, fa0
    ret
lr_stub_llvm_fabs_f64_end:

.globl lr_stub_llvm_sqrt_f32_begin
.globl lr_stub_llvm_sqrt_f32_end
lr_stub_llvm_sqrt_f32_begin:
    fsqrt.s fa0, fa0
    ret
lr_stub_llvm_sqrt_f32_end:

.globl lr_stub_llvm_sqrt_f64_begin
.globl lr_stub_llvm_sqrt_f64_end
lr_stub_llvm_sqrt_f64_begin:
    fsqrt.d fa0, fa0
    ret
lr_stub_llvm_sqrt_f64_end:

.globl lr_stub_llvm_copysign_f32_begin
.globl lr_stub_llvm_copysign_f32_end
lr_stub_llvm_copysign_f32_begin:
    fsgnj.s fa0, fa0, fa1
    ret
lr_stub_llvm_copysign_f32_end:

.globl lr_stub_llvm_copysign_f64_begin
.globl lr_stub_llvm_copysign_f64_end
lr_stub_llvm_copysign_f64_begin:
    fsgnj.d fa0, fa0, fa1
    ret
lr_stub_llvm_copysign_f64_end:

# exp(x) via Taylor series: sum_{k=0}^{N} x^k / k!
.globl lr_stub_llvm_exp_f64_begin
.globl lr_stub_llvm_exp_f64_end
lr_stub_llvm_exp_f64_begin:
    # fa0 = x, compute exp(x) via 24-term Taylor
    fmv.d ft0, fa0              # ft0 = x (copy)
    li t0, 0x3ff0000000000000   # 1.0
    fmv.d.x ft1, t0             # ft1 = term = 1.0
    fmv.d.x ft2, t0             # ft2 = sum = 1.0
    li t1, 1                    # t1 = k = 1
    li t2, 24                   # iterations
1:
    fmul.d ft1, ft1, ft0       # term *= x
    fcvt.d.l ft3, t1            # ft3 = (double)k
    fdiv.d ft1, ft1, ft3       # term /= k
    fadd.d ft2, ft2, ft1       # sum += term
    addi t1, t1, 1
    ble t1, t2, 1b
    fmv.d fa0, ft2
    ret
lr_stub_llvm_exp_f64_end:

.globl lr_stub_llvm_exp_f32_begin
.globl lr_stub_llvm_exp_f32_end
lr_stub_llvm_exp_f32_begin:
    fcvt.d.s ft0, fa0           # promote to double
    li t0, 0x3ff0000000000000
    fmv.d.x ft1, t0
    fmv.d.x ft2, t0
    li t1, 1
    li t2, 20
2:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, 2b
    fcvt.s.d fa0, ft2
    ret
lr_stub_llvm_exp_f32_end:

# pow(x, y): integer exponent via squaring, else exp(y * ln(x))
.globl lr_stub_llvm_pow_f64_begin
.globl lr_stub_llvm_pow_f64_end
lr_stub_llvm_pow_f64_begin:
    # fa0=x, fa1=y
    # Check if y is integer
    fcvt.l.d t0, fa1, rtz       # t0 = (int64_t)y
    fcvt.d.l ft0, t0            # ft0 = (double)(int64_t)y
    feq.d t1, ft0, fa1          # t1 = (ft0 == fa1)
    beqz t1, .Lpow_f64_frac

    # Integer path: binary exponentiation
    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3             # ft1 = result = 1.0
    bgez t0, .Lpow_f64_ipos
    # negative exponent: x = 1/x, n = -n
    fdiv.d fa0, ft1, fa0
    neg t0, t0
.Lpow_f64_ipos:
    beqz t0, .Lpow_f64_idone
.Lpow_f64_iloop:
    andi t1, t0, 1
    beqz t1, .Lpow_f64_isq
    fmul.d ft1, ft1, fa0
.Lpow_f64_isq:
    fmul.d fa0, fa0, fa0
    srli t0, t0, 1
    bnez t0, .Lpow_f64_iloop
.Lpow_f64_idone:
    fmv.d fa0, ft1
    ret

.Lpow_f64_frac:
    # x <= 0 -> return 0 for non-integer exponent
    li t3, 0x3ff0000000000000
    fmv.d.x ft4, t3             # ft4 = 1.0
    fmv.d.x ft5, zero           # ft5 = 0.0
    fle.d t1, fa0, ft5
    bnez t1, .Lpow_f64_ret_zero

    # ln(x) via atanh series: ln(x) = 2*atanh((x-1)/(x+1))
    fsub.d ft0, fa0, ft4       # x-1
    fadd.d ft1, fa0, ft4       # x+1
    fdiv.d ft2, ft0, ft1       # u = (x-1)/(x+1)
    fmul.d ft3, ft2, ft2       # u2 = u*u
    fmv.d ft4, ft2             # term = u
    fmv.d ft5, ft2             # sum = u
    li t1, 3
    li t2, 39
.Lpow_f64_ln_loop:
    fmul.d ft4, ft4, ft3       # term *= u2
    fcvt.d.l ft6, t1
    fdiv.d ft6, ft4, ft6       # term/k
    fadd.d ft5, ft5, ft6       # sum += term/k
    addi t1, t1, 2
    ble t1, t2, .Lpow_f64_ln_loop

    li t3, 0x4000000000000000   # 2.0
    fmv.d.x ft6, t3
    fmul.d ft5, ft5, ft6       # ln(x) = 2*sum
    fmul.d ft0, ft5, fa1       # ln(x)*y

    # exp(ln(x)*y)
    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    fmv.d.x ft2, t3
    li t1, 1
    li t2, 24
.Lpow_f64_exp_loop:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, .Lpow_f64_exp_loop
    fmv.d fa0, ft2
    ret

.Lpow_f64_ret_zero:
    fmv.d.x fa0, zero
    ret
lr_stub_llvm_pow_f64_end:

.globl lr_stub_llvm_pow_f32_begin
.globl lr_stub_llvm_pow_f32_end
lr_stub_llvm_pow_f32_begin:
    fcvt.d.s ft0, fa0           # x in double
    fcvt.d.s ft1, fa1           # y in double
    fmv.d fa0, ft0
    fmv.d fa1, ft1

    fcvt.l.d t0, fa1, rtz
    fcvt.d.l ft0, t0
    feq.d t1, ft0, fa1
    beqz t1, .Lpow_f32_frac

    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    bgez t0, .Lpow_f32_ipos
    fdiv.d fa0, ft1, fa0
    neg t0, t0
.Lpow_f32_ipos:
    beqz t0, .Lpow_f32_idone
.Lpow_f32_iloop:
    andi t1, t0, 1
    beqz t1, .Lpow_f32_isq
    fmul.d ft1, ft1, fa0
.Lpow_f32_isq:
    fmul.d fa0, fa0, fa0
    srli t0, t0, 1
    bnez t0, .Lpow_f32_iloop
.Lpow_f32_idone:
    fcvt.s.d fa0, ft1
    ret

.Lpow_f32_frac:
    li t3, 0x3ff0000000000000
    fmv.d.x ft4, t3
    fmv.d.x ft5, zero
    fle.d t1, fa0, ft5
    bnez t1, .Lpow_f32_ret_zero

    fsub.d ft0, fa0, ft4
    fadd.d ft1, fa0, ft4
    fdiv.d ft2, ft0, ft1
    fmul.d ft3, ft2, ft2
    fmv.d ft4, ft2
    fmv.d ft5, ft2
    li t1, 3
    li t2, 31
.Lpow_f32_ln_loop:
    fmul.d ft4, ft4, ft3
    fcvt.d.l ft6, t1
    fdiv.d ft6, ft4, ft6
    fadd.d ft5, ft5, ft6
    addi t1, t1, 2
    ble t1, t2, .Lpow_f32_ln_loop

    li t3, 0x4000000000000000
    fmv.d.x ft6, t3
    fmul.d ft5, ft5, ft6
    fmul.d ft0, ft5, fa1

    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    fmv.d.x ft2, t3
    li t1, 1
    li t2, 20
.Lpow_f32_exp_loop:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, .Lpow_f32_exp_loop
    fcvt.s.d fa0, ft2
    ret

.Lpow_f32_ret_zero:
    fmv.s.x fa0, zero
    ret
lr_stub_llvm_pow_f32_end:

# powi(x, n): integer exponentiation via binary squaring
.globl lr_stub_llvm_powi_f32_i32_begin
.globl lr_stub_llvm_powi_f32_i32_end
lr_stub_llvm_powi_f32_i32_begin:
    # fa0=base, a0=exponent (i32)
    li t3, 0x3f800000
    fmv.s.x ft0, t3             # ft0 = 1.0f
    sext.w t0, a0                # sign-extend i32
    bgez t0, .Lpowi_f32_i32_pos
    li t3, 0x3f800000
    fmv.s.x ft1, t3
    fdiv.s fa0, ft1, fa0
    neg t0, t0
.Lpowi_f32_i32_pos:
    beqz t0, .Lpowi_f32_i32_done
.Lpowi_f32_i32_loop:
    andi t1, t0, 1
    beqz t1, .Lpowi_f32_i32_sq
    fmul.s ft0, ft0, fa0
.Lpowi_f32_i32_sq:
    fmul.s fa0, fa0, fa0
    srli t0, t0, 1
    bnez t0, .Lpowi_f32_i32_loop
.Lpowi_f32_i32_done:
    fmv.s fa0, ft0
    ret
lr_stub_llvm_powi_f32_i32_end:

.globl lr_stub_llvm_powi_f64_i32_begin
.globl lr_stub_llvm_powi_f64_i32_end
lr_stub_llvm_powi_f64_i32_begin:
    li t3, 0x3ff0000000000000
    fmv.d.x ft0, t3
    sext.w t0, a0
    bgez t0, .Lpowi_f64_i32_pos
    fmv.d.x ft1, t3
    fdiv.d fa0, ft1, fa0
    neg t0, t0
.Lpowi_f64_i32_pos:
    beqz t0, .Lpowi_f64_i32_done
.Lpowi_f64_i32_loop:
    andi t1, t0, 1
    beqz t1, .Lpowi_f64_i32_sq
    fmul.d ft0, ft0, fa0
.Lpowi_f64_i32_sq:
    fmul.d fa0, fa0, fa0
    srli t0, t0, 1
    bnez t0, .Lpowi_f64_i32_loop
.Lpowi_f64_i32_done:
    fmv.d fa0, ft0
    ret
lr_stub_llvm_powi_f64_i32_end:

.globl lr_stub_llvm_powi_f32_i64_begin
.globl lr_stub_llvm_powi_f32_i64_end
lr_stub_llvm_powi_f32_i64_begin:
    li t3, 0x3f800000
    fmv.s.x ft0, t3
    mv t0, a0
    bgez t0, .Lpowi_f32_i64_pos
    li t3, 0x3f800000
    fmv.s.x ft1, t3
    fdiv.s fa0, ft1, fa0
    neg t0, t0
.Lpowi_f32_i64_pos:
    beqz t0, .Lpowi_f32_i64_done
.Lpowi_f32_i64_loop:
    andi t1, t0, 1
    beqz t1, .Lpowi_f32_i64_sq
    fmul.s ft0, ft0, fa0
.Lpowi_f32_i64_sq:
    fmul.s fa0, fa0, fa0
    srli t0, t0, 1
    bnez t0, .Lpowi_f32_i64_loop
.Lpowi_f32_i64_done:
    fmv.s fa0, ft0
    ret
lr_stub_llvm_powi_f32_i64_end:

.globl lr_stub_llvm_powi_f64_i64_begin
.globl lr_stub_llvm_powi_f64_i64_end
lr_stub_llvm_powi_f64_i64_begin:
    li t3, 0x3ff0000000000000
    fmv.d.x ft0, t3
    mv t0, a0
    bgez t0, .Lpowi_f64_i64_pos
    fmv.d.x ft1, t3
    fdiv.d fa0, ft1, fa0
    neg t0, t0
.Lpowi_f64_i64_pos:
    beqz t0, .Lpowi_f64_i64_done
.Lpowi_f64_i64_loop:
    andi t1, t0, 1
    beqz t1, .Lpowi_f64_i64_sq
    fmul.d ft0, ft0, fa0
.Lpowi_f64_i64_sq:
    fmul.d fa0, fa0, fa0
    srli t0, t0, 1
    bnez t0, .Lpowi_f64_i64_loop
.Lpowi_f64_i64_done:
    fmv.d fa0, ft0
    ret
lr_stub_llvm_powi_f64_i64_end:

# memset(dest, val, n) -- byte loop
.globl lr_stub_llvm_memset_i32_begin
.globl lr_stub_llvm_memset_i32_end
lr_stub_llvm_memset_i32_begin:
    # a0=dest, a1=val(i8), a2=count(i32)
    sext.w a2, a2
    blez a2, .Lmemset_i32_done
    andi a1, a1, 0xff
.Lmemset_i32_loop:
    sb a1, 0(a0)
    addi a0, a0, 1
    addi a2, a2, -1
    bnez a2, .Lmemset_i32_loop
.Lmemset_i32_done:
    ret
lr_stub_llvm_memset_i32_end:

.globl lr_stub_llvm_memset_i64_begin
.globl lr_stub_llvm_memset_i64_end
lr_stub_llvm_memset_i64_begin:
    blez a2, .Lmemset_i64_done
    andi a1, a1, 0xff
.Lmemset_i64_loop:
    sb a1, 0(a0)
    addi a0, a0, 1
    addi a2, a2, -1
    bnez a2, .Lmemset_i64_loop
.Lmemset_i64_done:
    ret
lr_stub_llvm_memset_i64_end:

# memcpy(dest, src, n) -- byte loop, no overlap handling
.globl lr_stub_llvm_memcpy_i32_begin
.globl lr_stub_llvm_memcpy_i32_end
lr_stub_llvm_memcpy_i32_begin:
    sext.w a2, a2
    blez a2, .Lmemcpy_i32_done
.Lmemcpy_i32_loop:
    lb t0, 0(a1)
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lmemcpy_i32_loop
.Lmemcpy_i32_done:
    ret
lr_stub_llvm_memcpy_i32_end:

.globl lr_stub_llvm_memcpy_i64_begin
.globl lr_stub_llvm_memcpy_i64_end
lr_stub_llvm_memcpy_i64_begin:
    blez a2, .Lmemcpy_i64_done
.Lmemcpy_i64_loop:
    lb t0, 0(a1)
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lmemcpy_i64_loop
.Lmemcpy_i64_done:
    ret
lr_stub_llvm_memcpy_i64_end:

# memmove(dest, src, n) -- handles overlap
.globl lr_stub_llvm_memmove_i32_begin
.globl lr_stub_llvm_memmove_i32_end
lr_stub_llvm_memmove_i32_begin:
    sext.w a2, a2
    blez a2, .Lmemmove_i32_done
    beq a0, a1, .Lmemmove_i32_done
    bltu a0, a1, .Lmemmove_i32_fwd
    # Check overlap: if dest < src+n, copy backward
    add t0, a1, a2
    bgeu a0, t0, .Lmemmove_i32_fwd
    # Backward copy
    addi t1, a2, -1
.Lmemmove_i32_bwd:
    add t2, a1, t1
    lb t3, 0(t2)
    add t2, a0, t1
    sb t3, 0(t2)
    addi t1, t1, -1
    bgez t1, .Lmemmove_i32_bwd
    j .Lmemmove_i32_done
.Lmemmove_i32_fwd:
    lb t0, 0(a1)
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lmemmove_i32_fwd
.Lmemmove_i32_done:
    ret
lr_stub_llvm_memmove_i32_end:

.globl lr_stub_llvm_memmove_i64_begin
.globl lr_stub_llvm_memmove_i64_end
lr_stub_llvm_memmove_i64_begin:
    blez a2, .Lmemmove_i64_done
    beq a0, a1, .Lmemmove_i64_done
    bltu a0, a1, .Lmemmove_i64_fwd
    add t0, a1, a2
    bgeu a0, t0, .Lmemmove_i64_fwd
    addi t1, a2, -1
.Lmemmove_i64_bwd:
    add t2, a1, t1
    lb t3, 0(t2)
    add t2, a0, t1
    sb t3, 0(t2)
    addi t1, t1, -1
    bgez t1, .Lmemmove_i64_bwd
    j .Lmemmove_i64_done
.Lmemmove_i64_fwd:
    lb t0, 0(a1)
    sb t0, 0(a0)
    addi a0, a0, 1
    addi a1, a1, 1
    addi a2, a2, -1
    bnez a2, .Lmemmove_i64_fwd
.Lmemmove_i64_done:
    ret
lr_stub_llvm_memmove_i64_end:

# floor: convert to int with round-toward-negative-infinity, convert back
.globl lr_stub_llvm_floor_f32_begin
.globl lr_stub_llvm_floor_f32_end
lr_stub_llvm_floor_f32_begin:
    fcvt.w.s t0, fa0, rdn
    fcvt.s.w fa0, t0
    ret
lr_stub_llvm_floor_f32_end:

.globl lr_stub_llvm_floor_f64_begin
.globl lr_stub_llvm_floor_f64_end
lr_stub_llvm_floor_f64_begin:
    fcvt.l.d t0, fa0, rdn
    fcvt.d.l fa0, t0
    ret
lr_stub_llvm_floor_f64_end:

.globl lr_stub_llvm_ceil_f32_begin
.globl lr_stub_llvm_ceil_f32_end
lr_stub_llvm_ceil_f32_begin:
    fcvt.w.s t0, fa0, rup
    fcvt.s.w fa0, t0
    ret
lr_stub_llvm_ceil_f32_end:

.globl lr_stub_llvm_ceil_f64_begin
.globl lr_stub_llvm_ceil_f64_end
lr_stub_llvm_ceil_f64_begin:
    fcvt.l.d t0, fa0, rup
    fcvt.d.l fa0, t0
    ret
lr_stub_llvm_ceil_f64_end:

.globl lr_stub_llvm_trunc_f32_begin
.globl lr_stub_llvm_trunc_f32_end
lr_stub_llvm_trunc_f32_begin:
    fcvt.w.s t0, fa0, rtz
    fcvt.s.w fa0, t0
    ret
lr_stub_llvm_trunc_f32_end:

.globl lr_stub_llvm_trunc_f64_begin
.globl lr_stub_llvm_trunc_f64_end
lr_stub_llvm_trunc_f64_begin:
    fcvt.l.d t0, fa0, rtz
    fcvt.d.l fa0, t0
    ret
lr_stub_llvm_trunc_f64_end:

# round: round to nearest, ties away from zero (rmm)
.globl lr_stub_llvm_round_f32_begin
.globl lr_stub_llvm_round_f32_end
lr_stub_llvm_round_f32_begin:
    fcvt.w.s t0, fa0, rmm
    fcvt.s.w fa0, t0
    ret
lr_stub_llvm_round_f32_end:

.globl lr_stub_llvm_round_f64_begin
.globl lr_stub_llvm_round_f64_end
lr_stub_llvm_round_f64_begin:
    fcvt.l.d t0, fa0, rmm
    fcvt.d.l fa0, t0
    ret
lr_stub_llvm_round_f64_end:

# rint: round to nearest, ties to even (rne) -- may raise inexact
.globl lr_stub_llvm_rint_f32_begin
.globl lr_stub_llvm_rint_f32_end
lr_stub_llvm_rint_f32_begin:
    fcvt.w.s t0, fa0, rne
    fcvt.s.w fa0, t0
    ret
lr_stub_llvm_rint_f32_end:

.globl lr_stub_llvm_rint_f64_begin
.globl lr_stub_llvm_rint_f64_end
lr_stub_llvm_rint_f64_begin:
    fcvt.l.d t0, fa0, rne
    fcvt.d.l fa0, t0
    ret
lr_stub_llvm_rint_f64_end:

# nearbyint: same as rint for our purposes (rne)
.globl lr_stub_llvm_nearbyint_f32_begin
.globl lr_stub_llvm_nearbyint_f32_end
lr_stub_llvm_nearbyint_f32_begin:
    fcvt.w.s t0, fa0, rne
    fcvt.s.w fa0, t0
    ret
lr_stub_llvm_nearbyint_f32_end:

.globl lr_stub_llvm_nearbyint_f64_begin
.globl lr_stub_llvm_nearbyint_f64_end
lr_stub_llvm_nearbyint_f64_begin:
    fcvt.l.d t0, fa0, rne
    fcvt.d.l fa0, t0
    ret
lr_stub_llvm_nearbyint_f64_end:

# fma: fused multiply-add  fa0 = fa0*fa1 + fa2
.globl lr_stub_llvm_fma_f32_begin
.globl lr_stub_llvm_fma_f32_end
lr_stub_llvm_fma_f32_begin:
    fmadd.s fa0, fa0, fa1, fa2
    ret
lr_stub_llvm_fma_f32_end:

.globl lr_stub_llvm_fma_f64_begin
.globl lr_stub_llvm_fma_f64_end
lr_stub_llvm_fma_f64_begin:
    fmadd.d fa0, fa0, fa1, fa2
    ret
lr_stub_llvm_fma_f64_end:

# fmuladd vector: scalar loops (no V extension assumed)
# v2f32: a[0..1] * b[0..1] + c[0..1], packed in fa0/fa1/fa2 as pairs
# RISC-V passes vectors via GP registers: a0/a1 = operands
# For v2f32: a0 = {x0,x1} packed as 2xf32 in 64-bit reg
# We unpack, multiply-add, repack.
.globl lr_stub_llvm_fmuladd_v2f32_begin
.globl lr_stub_llvm_fmuladd_v2f32_end
lr_stub_llvm_fmuladd_v2f32_begin:
    # a0={a0_lo,a0_hi}, a1={b0_lo,b0_hi}, a2={c0_lo,c0_hi}
    # Extract low f32 from a0
    fmv.w.x ft0, a0             # a[0]
    srli t0, a0, 32
    fmv.w.x ft1, t0             # a[1]
    fmv.w.x ft2, a1             # b[0]
    srli t0, a1, 32
    fmv.w.x ft3, t0             # b[1]
    fmv.w.x ft4, a2             # c[0]
    srli t0, a2, 32
    fmv.w.x ft5, t0             # c[1]
    fmadd.s ft0, ft0, ft2, ft4  # r[0]
    fmadd.s ft1, ft1, ft3, ft5  # r[1]
    fmv.x.w t0, ft0
    fmv.x.w t1, ft1
    slli t1, t1, 32
    or a0, t0, t1
    ret
lr_stub_llvm_fmuladd_v2f32_end:

.globl lr_stub_llvm_fmuladd_v4f32_begin
.globl lr_stub_llvm_fmuladd_v4f32_end
lr_stub_llvm_fmuladd_v4f32_begin:
    # a0={a0,a1}, a1={a2,a3}, a2={b0,b1}, a3={b2,b3}, a4={c0,c1}, a5={c2,c3}
    fmv.w.x ft0, a0
    srli t0, a0, 32
    fmv.w.x ft1, t0
    fmv.w.x ft2, a1
    srli t0, a1, 32
    fmv.w.x ft3, t0

    fmv.w.x ft4, a2
    srli t0, a2, 32
    fmv.w.x ft5, t0
    fmv.w.x ft6, a3
    srli t0, a3, 32
    fmv.w.x ft7, t0

    fmv.w.x ft8, a4
    srli t0, a4, 32
    fmv.w.x ft9, t0
    fmv.w.x ft10, a5
    srli t0, a5, 32
    fmv.w.x ft11, t0

    fmadd.s ft0, ft0, ft4, ft8
    fmadd.s ft1, ft1, ft5, ft9
    fmadd.s ft2, ft2, ft6, ft10
    fmadd.s ft3, ft3, ft7, ft11

    fmv.x.w t0, ft0
    fmv.x.w t1, ft1
    slli t1, t1, 32
    or a0, t0, t1
    fmv.x.w t0, ft2
    fmv.x.w t1, ft3
    slli t1, t1, 32
    or a1, t0, t1
    ret
lr_stub_llvm_fmuladd_v4f32_end:

.globl lr_stub_llvm_fmuladd_v2f64_begin
.globl lr_stub_llvm_fmuladd_v2f64_end
lr_stub_llvm_fmuladd_v2f64_begin:
    # a0=a[0], a1=a[1], a2=b[0], a3=b[1], a4=c[0], a5=c[1]
    fmv.d.x ft0, a0
    fmv.d.x ft1, a1
    fmv.d.x ft2, a2
    fmv.d.x ft3, a3
    fmv.d.x ft4, a4
    fmv.d.x ft5, a5
    fmadd.d ft0, ft0, ft2, ft4
    fmadd.d ft1, ft1, ft3, ft5
    fmv.x.d a0, ft0
    fmv.x.d a1, ft1
    ret
lr_stub_llvm_fmuladd_v2f64_end:

# minnum / maxnum
.globl lr_stub_llvm_minnum_f32_begin
.globl lr_stub_llvm_minnum_f32_end
lr_stub_llvm_minnum_f32_begin:
    fmin.s fa0, fa0, fa1
    ret
lr_stub_llvm_minnum_f32_end:

.globl lr_stub_llvm_minnum_f64_begin
.globl lr_stub_llvm_minnum_f64_end
lr_stub_llvm_minnum_f64_begin:
    fmin.d fa0, fa0, fa1
    ret
lr_stub_llvm_minnum_f64_end:

.globl lr_stub_llvm_maxnum_f32_begin
.globl lr_stub_llvm_maxnum_f32_end
lr_stub_llvm_maxnum_f32_begin:
    fmax.s fa0, fa0, fa1
    ret
lr_stub_llvm_maxnum_f32_end:

.globl lr_stub_llvm_maxnum_f64_begin
.globl lr_stub_llvm_maxnum_f64_end
lr_stub_llvm_maxnum_f64_begin:
    fmax.d fa0, fa0, fa1
    ret
lr_stub_llvm_maxnum_f64_end:

# abs integer: branchless srai+xor+sub
.globl lr_stub_llvm_abs_i8_begin
.globl lr_stub_llvm_abs_i8_end
lr_stub_llvm_abs_i8_begin:
    slli a0, a0, 56
    srai a0, a0, 56              # sign-extend i8
    srai t0, a0, 63
    xor a0, a0, t0
    sub a0, a0, t0
    ret
lr_stub_llvm_abs_i8_end:

.globl lr_stub_llvm_abs_i16_begin
.globl lr_stub_llvm_abs_i16_end
lr_stub_llvm_abs_i16_begin:
    slli a0, a0, 48
    srai a0, a0, 48              # sign-extend i16
    srai t0, a0, 63
    xor a0, a0, t0
    sub a0, a0, t0
    ret
lr_stub_llvm_abs_i16_end:

.globl lr_stub_llvm_abs_i32_begin
.globl lr_stub_llvm_abs_i32_end
lr_stub_llvm_abs_i32_begin:
    sext.w a0, a0
    sraiw t0, a0, 31
    xor a0, a0, t0
    subw a0, a0, t0
    ret
lr_stub_llvm_abs_i32_end:

.globl lr_stub_llvm_abs_i64_begin
.globl lr_stub_llvm_abs_i64_end
lr_stub_llvm_abs_i64_begin:
    srai t0, a0, 63
    xor a0, a0, t0
    sub a0, a0, t0
    ret
lr_stub_llvm_abs_i64_end:

# assume: no-op
.globl lr_stub_llvm_assume_begin
.globl lr_stub_llvm_assume_end
lr_stub_llvm_assume_begin:
    ret
lr_stub_llvm_assume_end:

# trap
.globl lr_stub_llvm_trap_begin
.globl lr_stub_llvm_trap_end
lr_stub_llvm_trap_begin:
    ebreak
    ret
lr_stub_llvm_trap_end:

# sin(x) via range reduction + Taylor polynomial
.globl lr_stub_llvm_sin_f64_begin
.globl lr_stub_llvm_sin_f64_end
lr_stub_llvm_sin_f64_begin:
    # Range reduction: x = x - round(x/(2*pi)) * 2*pi
    1: auipc t0, %pcrel_hi(.Lsin_f64_inv_two_pi)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d ft1, fa0, ft0        # x / (2*pi)
    li t1, 0x3ff0000000000000
    fmv.d.x ft2, t1             # 0.5 for rounding
    # round to nearest: add 0.5, truncate (for positive); handle negative
    fcvt.l.d t1, ft1, rne       # nearest int
    fcvt.d.l ft1, t1            # back to double
    1: auipc t0, %pcrel_hi(.Lsin_f64_two_pi)
    fld ft2, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft2
    fsub.d fa0, fa0, ft1        # x reduced

    fmul.d ft0, fa0, fa0        # x^2

    # Horner: c11*x^2 + c9, *x^2 + c7, *x^2 + c5, *x^2 + c3
    1: auipc t0, %pcrel_hi(.Lsin_f64_c11)
    fld ft1, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f64_c9)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f64_c7)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f64_c5)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f64_c3)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2

    fmul.d ft1, ft1, ft0        # poly * x^2
    fmul.d ft1, ft1, fa0        # poly * x^2 * x
    fadd.d fa0, fa0, ft1        # x + poly*x^3
    ret
    .balign 8
.Lsin_f64_inv_two_pi:
    .double 0.15915494309189533577
.Lsin_f64_two_pi:
    .double 6.28318530717958647692
.Lsin_f64_c3:
    .double -0.16666666666666666667
.Lsin_f64_c5:
    .double 0.00833333333333333333
.Lsin_f64_c7:
    .double -0.00019841269841269841
.Lsin_f64_c9:
    .double 0.00000275573192239859
.Lsin_f64_c11:
    .double -0.00000002505210838544
lr_stub_llvm_sin_f64_end:

.globl lr_stub_llvm_sin_f32_begin
.globl lr_stub_llvm_sin_f32_end
lr_stub_llvm_sin_f32_begin:
    fcvt.d.s fa0, fa0
    1: auipc t0, %pcrel_hi(.Lsin_f32_inv_two_pi)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d ft1, fa0, ft0
    fcvt.l.d t1, ft1, rne
    fcvt.d.l ft1, t1
    1: auipc t0, %pcrel_hi(.Lsin_f32_two_pi)
    fld ft2, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft2
    fsub.d fa0, fa0, ft1

    fmul.d ft0, fa0, fa0

    1: auipc t0, %pcrel_hi(.Lsin_f32_c11)
    fld ft1, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f32_c9)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f32_c7)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f32_c5)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lsin_f32_c3)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2

    fmul.d ft1, ft1, ft0
    fmul.d ft1, ft1, fa0
    fadd.d fa0, fa0, ft1
    fcvt.s.d fa0, fa0
    ret
    .balign 8
.Lsin_f32_inv_two_pi:
    .double 0.15915494309189533577
.Lsin_f32_two_pi:
    .double 6.28318530717958647692
.Lsin_f32_c3:
    .double -0.16666666666666666667
.Lsin_f32_c5:
    .double 0.00833333333333333333
.Lsin_f32_c7:
    .double -0.00019841269841269841
.Lsin_f32_c9:
    .double 0.00000275573192239859
.Lsin_f32_c11:
    .double -0.00000002505210838544
lr_stub_llvm_sin_f32_end:

# cos(x) via range reduction + Taylor polynomial
.globl lr_stub_llvm_cos_f64_begin
.globl lr_stub_llvm_cos_f64_end
lr_stub_llvm_cos_f64_begin:
    1: auipc t0, %pcrel_hi(.Lcos_f64_inv_two_pi)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d ft1, fa0, ft0
    fcvt.l.d t1, ft1, rne
    fcvt.d.l ft1, t1
    1: auipc t0, %pcrel_hi(.Lcos_f64_two_pi)
    fld ft2, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft2
    fsub.d fa0, fa0, ft1

    fmul.d ft0, fa0, fa0

    1: auipc t0, %pcrel_hi(.Lcos_f64_c10)
    fld ft1, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f64_c8)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f64_c6)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f64_c4)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f64_c2)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2

    fmul.d ft1, ft1, ft0        # poly * x^2
    li t1, 0x3ff0000000000000
    fmv.d.x ft2, t1             # 1.0
    fadd.d fa0, ft2, ft1
    ret
    .balign 8
.Lcos_f64_inv_two_pi:
    .double 0.15915494309189533577
.Lcos_f64_two_pi:
    .double 6.28318530717958647692
.Lcos_f64_c2:
    .double -0.5
.Lcos_f64_c4:
    .double 0.04166666666666666667
.Lcos_f64_c6:
    .double -0.00138888888888888889
.Lcos_f64_c8:
    .double 0.00002480158730158730
.Lcos_f64_c10:
    .double -0.00000027557319223986
lr_stub_llvm_cos_f64_end:

.globl lr_stub_llvm_cos_f32_begin
.globl lr_stub_llvm_cos_f32_end
lr_stub_llvm_cos_f32_begin:
    fcvt.d.s fa0, fa0
    1: auipc t0, %pcrel_hi(.Lcos_f32_inv_two_pi)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d ft1, fa0, ft0
    fcvt.l.d t1, ft1, rne
    fcvt.d.l ft1, t1
    1: auipc t0, %pcrel_hi(.Lcos_f32_two_pi)
    fld ft2, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft2
    fsub.d fa0, fa0, ft1

    fmul.d ft0, fa0, fa0

    1: auipc t0, %pcrel_hi(.Lcos_f32_c10)
    fld ft1, %pcrel_lo(1b)(t0)
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f32_c8)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f32_c6)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f32_c4)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2
    fmul.d ft1, ft1, ft0
    1: auipc t0, %pcrel_hi(.Lcos_f32_c2)
    fld ft2, %pcrel_lo(1b)(t0)
    fadd.d ft1, ft1, ft2

    fmul.d ft1, ft1, ft0
    li t1, 0x3ff0000000000000
    fmv.d.x ft2, t1
    fadd.d fa0, ft2, ft1
    fcvt.s.d fa0, fa0
    ret
    .balign 8
.Lcos_f32_inv_two_pi:
    .double 0.15915494309189533577
.Lcos_f32_two_pi:
    .double 6.28318530717958647692
.Lcos_f32_c2:
    .double -0.5
.Lcos_f32_c4:
    .double 0.04166666666666666667
.Lcos_f32_c6:
    .double -0.00138888888888888889
.Lcos_f32_c8:
    .double 0.00002480158730158730
.Lcos_f32_c10:
    .double -0.00000027557319223986
lr_stub_llvm_cos_f32_end:

# log(x) via atanh series: ln(x) = 2*atanh((x-1)/(x+1))
.globl lr_stub_llvm_log_f64_begin
.globl lr_stub_llvm_log_f64_end
lr_stub_llvm_log_f64_begin:
    fmv.d.x ft0, zero
    fle.d t0, fa0, ft0
    bnez t0, .Llog_f64_nonpos

    li t1, 0x3ff0000000000000
    fmv.d.x ft0, t1             # 1.0
    fsub.d ft1, fa0, ft0        # x-1
    fadd.d ft2, fa0, ft0        # x+1
    fdiv.d ft3, ft1, ft2        # u
    fmul.d ft4, ft3, ft3        # u2
    fmv.d ft5, ft3              # term = u
    fmv.d ft6, ft3              # sum = u
    li t1, 3
    li t2, 39
.Llog_f64_loop:
    fmul.d ft5, ft5, ft4        # term *= u2
    fcvt.d.l ft7, t1
    fdiv.d ft7, ft5, ft7        # term/k
    fadd.d ft6, ft6, ft7
    addi t1, t1, 2
    ble t1, t2, .Llog_f64_loop
    li t1, 0x4000000000000000   # 2.0
    fmv.d.x ft7, t1
    fmul.d fa0, ft6, ft7
    ret
.Llog_f64_nonpos:
    fmv.d.x fa0, zero
    ret
lr_stub_llvm_log_f64_end:

.globl lr_stub_llvm_log_f32_begin
.globl lr_stub_llvm_log_f32_end
lr_stub_llvm_log_f32_begin:
    fcvt.d.s fa0, fa0
    fmv.d.x ft0, zero
    fle.d t0, fa0, ft0
    bnez t0, .Llog_f32_nonpos

    li t1, 0x3ff0000000000000
    fmv.d.x ft0, t1
    fsub.d ft1, fa0, ft0
    fadd.d ft2, fa0, ft0
    fdiv.d ft3, ft1, ft2
    fmul.d ft4, ft3, ft3
    fmv.d ft5, ft3
    fmv.d ft6, ft3
    li t1, 3
    li t2, 31
.Llog_f32_loop:
    fmul.d ft5, ft5, ft4
    fcvt.d.l ft7, t1
    fdiv.d ft7, ft5, ft7
    fadd.d ft6, ft6, ft7
    addi t1, t1, 2
    ble t1, t2, .Llog_f32_loop
    li t1, 0x4000000000000000
    fmv.d.x ft7, t1
    fmul.d fa0, ft6, ft7
    fcvt.s.d fa0, fa0
    ret
.Llog_f32_nonpos:
    fmv.s.x fa0, zero
    ret
lr_stub_llvm_log_f32_end:

# log2(x) = log(x) * (1/ln(2))
.globl lr_stub_llvm_log2_f64_begin
.globl lr_stub_llvm_log2_f64_end
lr_stub_llvm_log2_f64_begin:
    fmv.d.x ft0, zero
    fle.d t0, fa0, ft0
    bnez t0, .Llog2_f64_nonpos

    li t1, 0x3ff0000000000000
    fmv.d.x ft0, t1
    fsub.d ft1, fa0, ft0
    fadd.d ft2, fa0, ft0
    fdiv.d ft3, ft1, ft2
    fmul.d ft4, ft3, ft3
    fmv.d ft5, ft3
    fmv.d ft6, ft3
    li t1, 3
    li t2, 39
.Llog2_f64_loop:
    fmul.d ft5, ft5, ft4
    fcvt.d.l ft7, t1
    fdiv.d ft7, ft5, ft7
    fadd.d ft6, ft6, ft7
    addi t1, t1, 2
    ble t1, t2, .Llog2_f64_loop
    li t1, 0x4000000000000000
    fmv.d.x ft7, t1
    fmul.d ft6, ft6, ft7        # ln(x)
    1: auipc t0, %pcrel_hi(.Llog2_f64_inv_ln2)
    fld ft7, %pcrel_lo(1b)(t0)
    fmul.d fa0, ft6, ft7
    ret
.Llog2_f64_nonpos:
    fmv.d.x fa0, zero
    ret
    .balign 8
.Llog2_f64_inv_ln2:
    .double 1.44269504088896340736
lr_stub_llvm_log2_f64_end:

.globl lr_stub_llvm_log2_f32_begin
.globl lr_stub_llvm_log2_f32_end
lr_stub_llvm_log2_f32_begin:
    fcvt.d.s fa0, fa0
    fmv.d.x ft0, zero
    fle.d t0, fa0, ft0
    bnez t0, .Llog2_f32_nonpos

    li t1, 0x3ff0000000000000
    fmv.d.x ft0, t1
    fsub.d ft1, fa0, ft0
    fadd.d ft2, fa0, ft0
    fdiv.d ft3, ft1, ft2
    fmul.d ft4, ft3, ft3
    fmv.d ft5, ft3
    fmv.d ft6, ft3
    li t1, 3
    li t2, 31
.Llog2_f32_loop:
    fmul.d ft5, ft5, ft4
    fcvt.d.l ft7, t1
    fdiv.d ft7, ft5, ft7
    fadd.d ft6, ft6, ft7
    addi t1, t1, 2
    ble t1, t2, .Llog2_f32_loop
    li t1, 0x4000000000000000
    fmv.d.x ft7, t1
    fmul.d ft6, ft6, ft7
    1: auipc t0, %pcrel_hi(.Llog2_f32_inv_ln2)
    fld ft7, %pcrel_lo(1b)(t0)
    fmul.d fa0, ft6, ft7
    fcvt.s.d fa0, fa0
    ret
.Llog2_f32_nonpos:
    fmv.s.x fa0, zero
    ret
    .balign 8
.Llog2_f32_inv_ln2:
    .double 1.44269504088896340736
lr_stub_llvm_log2_f32_end:

# log10(x) = log(x) * (1/ln(10))
.globl lr_stub_llvm_log10_f64_begin
.globl lr_stub_llvm_log10_f64_end
lr_stub_llvm_log10_f64_begin:
    fmv.d.x ft0, zero
    fle.d t0, fa0, ft0
    bnez t0, .Llog10_f64_nonpos

    li t1, 0x3ff0000000000000
    fmv.d.x ft0, t1
    fsub.d ft1, fa0, ft0
    fadd.d ft2, fa0, ft0
    fdiv.d ft3, ft1, ft2
    fmul.d ft4, ft3, ft3
    fmv.d ft5, ft3
    fmv.d ft6, ft3
    li t1, 3
    li t2, 39
.Llog10_f64_loop:
    fmul.d ft5, ft5, ft4
    fcvt.d.l ft7, t1
    fdiv.d ft7, ft5, ft7
    fadd.d ft6, ft6, ft7
    addi t1, t1, 2
    ble t1, t2, .Llog10_f64_loop
    li t1, 0x4000000000000000
    fmv.d.x ft7, t1
    fmul.d ft6, ft6, ft7
    1: auipc t0, %pcrel_hi(.Llog10_f64_inv_ln10)
    fld ft7, %pcrel_lo(1b)(t0)
    fmul.d fa0, ft6, ft7
    ret
.Llog10_f64_nonpos:
    fmv.d.x fa0, zero
    ret
    .balign 8
.Llog10_f64_inv_ln10:
    .double 0.43429448190325182765
lr_stub_llvm_log10_f64_end:

.globl lr_stub_llvm_log10_f32_begin
.globl lr_stub_llvm_log10_f32_end
lr_stub_llvm_log10_f32_begin:
    fcvt.d.s fa0, fa0
    fmv.d.x ft0, zero
    fle.d t0, fa0, ft0
    bnez t0, .Llog10_f32_nonpos

    li t1, 0x3ff0000000000000
    fmv.d.x ft0, t1
    fsub.d ft1, fa0, ft0
    fadd.d ft2, fa0, ft0
    fdiv.d ft3, ft1, ft2
    fmul.d ft4, ft3, ft3
    fmv.d ft5, ft3
    fmv.d ft6, ft3
    li t1, 3
    li t2, 31
.Llog10_f32_loop:
    fmul.d ft5, ft5, ft4
    fcvt.d.l ft7, t1
    fdiv.d ft7, ft5, ft7
    fadd.d ft6, ft6, ft7
    addi t1, t1, 2
    ble t1, t2, .Llog10_f32_loop
    li t1, 0x4000000000000000
    fmv.d.x ft7, t1
    fmul.d ft6, ft6, ft7
    1: auipc t0, %pcrel_hi(.Llog10_f32_inv_ln10)
    fld ft7, %pcrel_lo(1b)(t0)
    fmul.d fa0, ft6, ft7
    fcvt.s.d fa0, fa0
    ret
.Llog10_f32_nonpos:
    fmv.s.x fa0, zero
    ret
    .balign 8
.Llog10_f32_inv_ln10:
    .double 0.43429448190325182765
lr_stub_llvm_log10_f32_end:

# exp2(x) = exp(x * ln(2))
.globl lr_stub_llvm_exp2_f64_begin
.globl lr_stub_llvm_exp2_f64_end
lr_stub_llvm_exp2_f64_begin:
    1: auipc t0, %pcrel_hi(.Lexp2_f64_ln2)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d fa0, fa0, ft0        # x * ln(2)
    # Taylor exp
    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    fmv.d.x ft2, t3
    fmv.d ft0, fa0               # save x*ln2
    li t1, 1
    li t2, 24
.Lexp2_f64_loop:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, .Lexp2_f64_loop
    fmv.d fa0, ft2
    ret
    .balign 8
.Lexp2_f64_ln2:
    .double 0.69314718055994530942
lr_stub_llvm_exp2_f64_end:

.globl lr_stub_llvm_exp2_f32_begin
.globl lr_stub_llvm_exp2_f32_end
lr_stub_llvm_exp2_f32_begin:
    fcvt.d.s fa0, fa0
    1: auipc t0, %pcrel_hi(.Lexp2_f32_ln2)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d fa0, fa0, ft0
    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    fmv.d.x ft2, t3
    fmv.d ft0, fa0
    li t1, 1
    li t2, 20
.Lexp2_f32_loop:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, .Lexp2_f32_loop
    fcvt.s.d fa0, ft2
    ret
    .balign 8
.Lexp2_f32_ln2:
    .double 0.69314718055994530942
lr_stub_llvm_exp2_f32_end:

# exp10(x) = exp(x * ln(10))
.globl lr_stub_llvm_exp10_f64_begin
.globl lr_stub_llvm_exp10_f64_end
lr_stub_llvm_exp10_f64_begin:
    1: auipc t0, %pcrel_hi(.Lexp10_f64_ln10)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d fa0, fa0, ft0
    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    fmv.d.x ft2, t3
    fmv.d ft0, fa0
    li t1, 1
    li t2, 24
.Lexp10_f64_loop:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, .Lexp10_f64_loop
    fmv.d fa0, ft2
    ret
    .balign 8
.Lexp10_f64_ln10:
    .double 2.30258509299404568402
lr_stub_llvm_exp10_f64_end:

.globl lr_stub_llvm_exp10_f32_begin
.globl lr_stub_llvm_exp10_f32_end
lr_stub_llvm_exp10_f32_begin:
    fcvt.d.s fa0, fa0
    1: auipc t0, %pcrel_hi(.Lexp10_f32_ln10)
    fld ft0, %pcrel_lo(1b)(t0)
    fmul.d fa0, fa0, ft0
    li t3, 0x3ff0000000000000
    fmv.d.x ft1, t3
    fmv.d.x ft2, t3
    fmv.d ft0, fa0
    li t1, 1
    li t2, 20
.Lexp10_f32_loop:
    fmul.d ft1, ft1, ft0
    fcvt.d.l ft3, t1
    fdiv.d ft1, ft1, ft3
    fadd.d ft2, ft2, ft1
    addi t1, t1, 1
    ble t1, t2, .Lexp10_f32_loop
    fcvt.s.d fa0, ft2
    ret
    .balign 8
.Lexp10_f32_ln10:
    .double 2.30258509299404568402
lr_stub_llvm_exp10_f32_end:

# is.fpclass: classify FP value
.globl lr_stub_llvm_is_fpclass_f32_begin
.globl lr_stub_llvm_is_fpclass_f32_end
lr_stub_llvm_is_fpclass_f32_begin:
    # fa0=value, a0=test mask
    fclass.s t1, fa0             # RISC-V fclass result in t1
    # Map RISC-V fclass bits to LLVM fpclass bits:
    # RV bit 0: -inf       -> LLVM 4 (neginf)
    # RV bit 1: -normal    -> LLVM 8 (negnormal)
    # RV bit 2: -subnormal -> LLVM 16 (negsubnormal)
    # RV bit 3: -zero      -> LLVM 32 (negzero)
    # RV bit 4: +zero      -> LLVM 64 (poszero)
    # RV bit 5: +subnormal -> LLVM 128 (possubnormal)
    # RV bit 6: +normal    -> LLVM 256 (posnormal)
    # RV bit 7: +inf       -> LLVM 512 (posinf)
    # RV bit 8: sNaN       -> LLVM 1
    # RV bit 9: qNaN       -> LLVM 2
    li t2, 0
    andi t3, t1, 0x001          # -inf
    beqz t3, 1f
    ori t2, t2, 4
1:  andi t3, t1, 0x002          # -normal
    beqz t3, 2f
    ori t2, t2, 8
2:  andi t3, t1, 0x004          # -subnormal
    beqz t3, 3f
    ori t2, t2, 16
3:  andi t3, t1, 0x008          # -zero
    beqz t3, 4f
    ori t2, t2, 32
4:  andi t3, t1, 0x010          # +zero
    beqz t3, 5f
    ori t2, t2, 64
5:  andi t3, t1, 0x020          # +subnormal
    beqz t3, 6f
    ori t2, t2, 128
6:  andi t3, t1, 0x040          # +normal
    beqz t3, 7f
    ori t2, t2, 256
7:  andi t3, t1, 0x080          # +inf
    beqz t3, 8f
    ori t2, t2, 512
8:  andi t3, t1, 0x100          # sNaN
    beqz t3, 9f
    ori t2, t2, 1
9:  andi t3, t1, 0x200          # qNaN
    beqz t3, 10f
    ori t2, t2, 2
10:
    and t2, t2, a0
    snez a0, t2
    ret
lr_stub_llvm_is_fpclass_f32_end:

.globl lr_stub_llvm_is_fpclass_f64_begin
.globl lr_stub_llvm_is_fpclass_f64_end
lr_stub_llvm_is_fpclass_f64_begin:
    fclass.d t1, fa0
    li t2, 0
    andi t3, t1, 0x001
    beqz t3, 1f
    ori t2, t2, 4
1:  andi t3, t1, 0x002
    beqz t3, 2f
    ori t2, t2, 8
2:  andi t3, t1, 0x004
    beqz t3, 3f
    ori t2, t2, 16
3:  andi t3, t1, 0x008
    beqz t3, 4f
    ori t2, t2, 32
4:  andi t3, t1, 0x010
    beqz t3, 5f
    ori t2, t2, 64
5:  andi t3, t1, 0x020
    beqz t3, 6f
    ori t2, t2, 128
6:  andi t3, t1, 0x040
    beqz t3, 7f
    ori t2, t2, 256
7:  andi t3, t1, 0x080
    beqz t3, 8f
    ori t2, t2, 512
8:  andi t3, t1, 0x100
    beqz t3, 9f
    ori t2, t2, 1
9:  andi t3, t1, 0x200
    beqz t3, 10f
    ori t2, t2, 2
10:
    and t2, t2, a0
    snez a0, t2
    ret
lr_stub_llvm_is_fpclass_f64_end:
