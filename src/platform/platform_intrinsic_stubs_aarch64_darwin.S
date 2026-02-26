.text

.globl _lr_stub_llvm_fabs_f32_begin
.globl _lr_stub_llvm_fabs_f32_end
_lr_stub_llvm_fabs_f32_begin:
    fabs s0, s0
    ret
_lr_stub_llvm_fabs_f32_end:

.globl _lr_stub_llvm_fabs_v2f32_begin
.globl _lr_stub_llvm_fabs_v2f32_end
_lr_stub_llvm_fabs_v2f32_begin:
    fabs v0.2s, v0.2s
    ret
_lr_stub_llvm_fabs_v2f32_end:

.globl _lr_stub_llvm_fabs_v2f64_begin
.globl _lr_stub_llvm_fabs_v2f64_end
_lr_stub_llvm_fabs_v2f64_begin:
    fabs v0.2d, v0.2d
    ret
_lr_stub_llvm_fabs_v2f64_end:

.globl _lr_stub_llvm_lifetime_start_begin
.globl _lr_stub_llvm_lifetime_start_end
_lr_stub_llvm_lifetime_start_begin:
    ret
_lr_stub_llvm_lifetime_start_end:

.globl _lr_stub_llvm_lifetime_end_begin
.globl _lr_stub_llvm_lifetime_end_end
_lr_stub_llvm_lifetime_end_begin:
    ret
_lr_stub_llvm_lifetime_end_end:

.globl _lr_stub_llvm_fabs_f64_begin
.globl _lr_stub_llvm_fabs_f64_end
_lr_stub_llvm_fabs_f64_begin:
    fabs d0, d0
    ret
_lr_stub_llvm_fabs_f64_end:

.globl _lr_stub_llvm_sqrt_f32_begin
.globl _lr_stub_llvm_sqrt_f32_end
_lr_stub_llvm_sqrt_f32_begin:
    fsqrt s0, s0
    ret
_lr_stub_llvm_sqrt_f32_end:

.globl _lr_stub_llvm_sqrt_f64_begin
.globl _lr_stub_llvm_sqrt_f64_end
_lr_stub_llvm_sqrt_f64_begin:
    fsqrt d0, d0
    ret
_lr_stub_llvm_sqrt_f64_end:

.globl _lr_stub_llvm_copysign_f32_begin
.globl _lr_stub_llvm_copysign_f32_end
_lr_stub_llvm_copysign_f32_begin:
    fmov w2, s0
    fmov w3, s1
    and w2, w2, #0x7fffffff
    and w3, w3, #0x80000000
    orr w2, w2, w3
    fmov s0, w2
    ret
_lr_stub_llvm_copysign_f32_end:

.globl _lr_stub_llvm_copysign_f64_begin
.globl _lr_stub_llvm_copysign_f64_end
_lr_stub_llvm_copysign_f64_begin:
    fmov x2, d0
    fmov x3, d1
    and x2, x2, #0x7fffffffffffffff
    and x3, x3, #0x8000000000000000
    orr x2, x2, x3
    fmov d0, x2
    ret
_lr_stub_llvm_copysign_f64_end:

.globl _lr_stub_llvm_exp_f64_begin
.globl _lr_stub_llvm_exp_f64_end
_lr_stub_llvm_exp_f64_begin:
    fmov d1, #1.0
    fmov d2, #1.0
    mov x9, #1
    mov x10, #24
1:
    scvtf d3, x9
    fmul d1, d1, d0
    fdiv d1, d1, d3
    fadd d2, d2, d1
    add x9, x9, #1
    cmp x9, x10
    ble 1b
    fmov d0, d2
    ret
_lr_stub_llvm_exp_f64_end:

.globl _lr_stub_llvm_exp_f32_begin
.globl _lr_stub_llvm_exp_f32_end
_lr_stub_llvm_exp_f32_begin:
    fcvt d0, s0
    fmov d1, #1.0
    fmov d2, #1.0
    mov x9, #1
    mov x10, #20
2:
    scvtf d3, x9
    fmul d1, d1, d0
    fdiv d1, d1, d3
    fadd d2, d2, d1
    add x9, x9, #1
    cmp x9, x10
    ble 2b
    fcvt s0, d2
    ret
_lr_stub_llvm_exp_f32_end:

.globl _lr_stub_llvm_pow_f64_begin
.globl _lr_stub_llvm_pow_f64_end
_lr_stub_llvm_pow_f64_begin:
    fcvtzs x9, d1
    scvtf d2, x9
    fcmp d2, d1
    b.ne 5f

    fmov d1, #1.0
    mov x10, x9
    cmp x10, #0
    b.ge 3f
    fmov d3, #1.0
    fdiv d0, d3, d0
    neg x10, x10
3:
    cbz x10, 4f
    tbz x10, #0, 6f
    fmul d1, d1, d0
6:
    fmul d0, d0, d0
    lsr x10, x10, #1
    cbnz x10, 3b
4:
    fmov d0, d1
    ret

5:
    fcmp d0, #0.0
    b.le 9f

    fmov d3, #1.0
    fsub d4, d0, d3
    fadd d5, d0, d3
    fdiv d6, d4, d5
    fmul d7, d6, d6
    fmov d8, d6
    fmov d9, d6

    mov x11, #3
    mov x12, #39
7:
    fmul d8, d8, d7
    scvtf d10, x11
    fdiv d10, d8, d10
    fadd d9, d9, d10
    add x11, x11, #2
    cmp x11, x12
    ble 7b

    fmov d10, #2.0
    fmul d9, d9, d10
    fmul d0, d9, d1

    fmov d1, #1.0
    fmov d2, #1.0
    mov x9, #1
    mov x10, #24
8:
    scvtf d3, x9
    fmul d1, d1, d0
    fdiv d1, d1, d3
    fadd d2, d2, d1
    add x9, x9, #1
    cmp x9, x10
    ble 8b
    fmov d0, d2
    ret

9:
    fmov d0, xzr
    ret
_lr_stub_llvm_pow_f64_end:

.globl _lr_stub_llvm_pow_f32_begin
.globl _lr_stub_llvm_pow_f32_end
_lr_stub_llvm_pow_f32_begin:
    fcvt d0, s0
    fcvt d1, s1

    fcvtzs x9, d1
    scvtf d2, x9
    fcmp d2, d1
    b.ne 15f

    fmov d1, #1.0
    mov x10, x9
    cmp x10, #0
    b.ge 13f
    fmov d3, #1.0
    fdiv d0, d3, d0
    neg x10, x10
13:
    cbz x10, 14f
    tbz x10, #0, 16f
    fmul d1, d1, d0
16:
    fmul d0, d0, d0
    lsr x10, x10, #1
    cbnz x10, 13b
14:
    fcvt s0, d1
    ret

15:
    fcmp d0, #0.0
    b.le 19f

    fmov d3, #1.0
    fsub d4, d0, d3
    fadd d5, d0, d3
    fdiv d6, d4, d5
    fmul d7, d6, d6
    fmov d8, d6
    fmov d9, d6

    mov x11, #3
    mov x12, #31
17:
    fmul d8, d8, d7
    scvtf d10, x11
    fdiv d10, d8, d10
    fadd d9, d9, d10
    add x11, x11, #2
    cmp x11, x12
    ble 17b

    fmov d10, #2.0
    fmul d9, d9, d10
    fmul d0, d9, d1

    fmov d1, #1.0
    fmov d2, #1.0
    mov x9, #1
    mov x10, #20
18:
    scvtf d3, x9
    fmul d1, d1, d0
    fdiv d1, d1, d3
    fadd d2, d2, d1
    add x9, x9, #1
    cmp x9, x10
    ble 18b
    fcvt s0, d2
    ret

19:
    fmov s0, wzr
    ret
_lr_stub_llvm_pow_f32_end:

.globl _lr_stub_llvm_powi_f32_i32_begin
.globl _lr_stub_llvm_powi_f32_i32_end
_lr_stub_llvm_powi_f32_i32_begin:
    fmov s1, #1.0
    sxtw x9, w0
    cmp x9, #0
    b.ge 20f
    fmov s2, #1.0
    fdiv s0, s2, s0
    neg x9, x9
20:
    cbz x9, 22f
21:
    tbz x9, #0, 23f
    fmul s1, s1, s0
23:
    fmul s0, s0, s0
    lsr x9, x9, #1
    cbnz x9, 21b
22:
    fmov s0, s1
    ret
_lr_stub_llvm_powi_f32_i32_end:

.globl _lr_stub_llvm_powi_f64_i32_begin
.globl _lr_stub_llvm_powi_f64_i32_end
_lr_stub_llvm_powi_f64_i32_begin:
    fmov d1, #1.0
    sxtw x9, w0
    cmp x9, #0
    b.ge 24f
    fmov d2, #1.0
    fdiv d0, d2, d0
    neg x9, x9
24:
    cbz x9, 26f
25:
    tbz x9, #0, 27f
    fmul d1, d1, d0
27:
    fmul d0, d0, d0
    lsr x9, x9, #1
    cbnz x9, 25b
26:
    fmov d0, d1
    ret
_lr_stub_llvm_powi_f64_i32_end:

.globl _lr_stub_llvm_powi_f32_i64_begin
.globl _lr_stub_llvm_powi_f32_i64_end
_lr_stub_llvm_powi_f32_i64_begin:
    fmov s1, #1.0
    mov x9, x0
    cmp x9, #0
    b.ge 28f
    adds x10, x9, #1
    neg x10, x10
    add x9, x10, #1
    fmov s2, #1.0
    fdiv s0, s2, s0
28:
    cbz x9, 30f
29:
    tbz x9, #0, 31f
    fmul s1, s1, s0
31:
    fmul s0, s0, s0
    lsr x9, x9, #1
    cbnz x9, 29b
30:
    fmov s0, s1
    ret
_lr_stub_llvm_powi_f32_i64_end:

.globl _lr_stub_llvm_powi_f64_i64_begin
.globl _lr_stub_llvm_powi_f64_i64_end
_lr_stub_llvm_powi_f64_i64_begin:
    fmov d1, #1.0
    mov x9, x0
    cmp x9, #0
    b.ge 32f
    adds x10, x9, #1
    neg x10, x10
    add x9, x10, #1
    fmov d2, #1.0
    fdiv d0, d2, d0
32:
    cbz x9, 34f
33:
    tbz x9, #0, 35f
    fmul d1, d1, d0
35:
    fmul d0, d0, d0
    lsr x9, x9, #1
    cbnz x9, 33b
34:
    fmov d0, d1
    ret
_lr_stub_llvm_powi_f64_i64_end:

.globl _lr_stub_llvm_memset_i32_begin
.globl _lr_stub_llvm_memset_i32_end
_lr_stub_llvm_memset_i32_begin:
    sxtw x2, w2
    cmp x2, #0
    ble 40f
    and w1, w1, #0xff
41:
    strb w1, [x0], #1
    subs x2, x2, #1
    b.ne 41b
40:
    ret
_lr_stub_llvm_memset_i32_end:

.globl _lr_stub_llvm_memset_i64_begin
.globl _lr_stub_llvm_memset_i64_end
_lr_stub_llvm_memset_i64_begin:
    cmp x2, #0
    ble 42f
    and w1, w1, #0xff
43:
    strb w1, [x0], #1
    subs x2, x2, #1
    b.ne 43b
42:
    ret
_lr_stub_llvm_memset_i64_end:

.globl _lr_stub_llvm_memcpy_i32_begin
.globl _lr_stub_llvm_memcpy_i32_end
_lr_stub_llvm_memcpy_i32_begin:
    sxtw x2, w2
    cmp x2, #0
    ble 44f
45:
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 45b
44:
    ret
_lr_stub_llvm_memcpy_i32_end:

.globl _lr_stub_llvm_memcpy_i64_begin
.globl _lr_stub_llvm_memcpy_i64_end
_lr_stub_llvm_memcpy_i64_begin:
    cmp x2, #0
    ble 46f
47:
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 47b
46:
    ret
_lr_stub_llvm_memcpy_i64_end:

.globl _lr_stub_llvm_memmove_i32_begin
.globl _lr_stub_llvm_memmove_i32_end
_lr_stub_llvm_memmove_i32_begin:
    sxtw x2, w2
    cmp x2, #0
    ble 50f
    cmp x0, x1
    beq 50f
    b.lo 51f
    add x3, x1, x2
    cmp x0, x3
    b.hs 51f
    sub x4, x2, #1
52:
    ldrb w5, [x1, x4]
    strb w5, [x0, x4]
    subs x4, x4, #1
    b.pl 52b
    b 50f
51:
53:
    ldrb w5, [x1], #1
    strb w5, [x0], #1
    subs x2, x2, #1
    b.ne 53b
50:
    ret
_lr_stub_llvm_memmove_i32_end:

.globl _lr_stub_llvm_memmove_i64_begin
.globl _lr_stub_llvm_memmove_i64_end
_lr_stub_llvm_memmove_i64_begin:
    cmp x2, #0
    ble 54f
    cmp x0, x1
    beq 54f
    b.lo 55f
    add x3, x1, x2
    cmp x0, x3
    b.hs 55f
    sub x4, x2, #1
56:
    ldrb w5, [x1, x4]
    strb w5, [x0, x4]
    subs x4, x4, #1
    b.pl 56b
    b 54f
55:
57:
    ldrb w5, [x1], #1
    strb w5, [x0], #1
    subs x2, x2, #1
    b.ne 57b
54:
    ret
_lr_stub_llvm_memmove_i64_end:

.globl _lr_stub_llvm_floor_f64_begin
.globl _lr_stub_llvm_floor_f64_end
_lr_stub_llvm_floor_f64_begin:
    frintm d0, d0
    ret
_lr_stub_llvm_floor_f64_end:

.globl _lr_stub_llvm_floor_f32_begin
.globl _lr_stub_llvm_floor_f32_end
_lr_stub_llvm_floor_f32_begin:
    frintm s0, s0
    ret
_lr_stub_llvm_floor_f32_end:

.globl _lr_stub_llvm_ceil_f64_begin
.globl _lr_stub_llvm_ceil_f64_end
_lr_stub_llvm_ceil_f64_begin:
    frintp d0, d0
    ret
_lr_stub_llvm_ceil_f64_end:

.globl _lr_stub_llvm_ceil_f32_begin
.globl _lr_stub_llvm_ceil_f32_end
_lr_stub_llvm_ceil_f32_begin:
    frintp s0, s0
    ret
_lr_stub_llvm_ceil_f32_end:

.globl _lr_stub_llvm_trunc_f64_begin
.globl _lr_stub_llvm_trunc_f64_end
_lr_stub_llvm_trunc_f64_begin:
    frintz d0, d0
    ret
_lr_stub_llvm_trunc_f64_end:

.globl _lr_stub_llvm_trunc_f32_begin
.globl _lr_stub_llvm_trunc_f32_end
_lr_stub_llvm_trunc_f32_begin:
    frintz s0, s0
    ret
_lr_stub_llvm_trunc_f32_end:

.globl _lr_stub_llvm_round_f64_begin
.globl _lr_stub_llvm_round_f64_end
_lr_stub_llvm_round_f64_begin:
    frinta d0, d0
    ret
_lr_stub_llvm_round_f64_end:

.globl _lr_stub_llvm_round_f32_begin
.globl _lr_stub_llvm_round_f32_end
_lr_stub_llvm_round_f32_begin:
    frinta s0, s0
    ret
_lr_stub_llvm_round_f32_end:

.globl _lr_stub_llvm_rint_f64_begin
.globl _lr_stub_llvm_rint_f64_end
_lr_stub_llvm_rint_f64_begin:
    frintx d0, d0
    ret
_lr_stub_llvm_rint_f64_end:

.globl _lr_stub_llvm_rint_f32_begin
.globl _lr_stub_llvm_rint_f32_end
_lr_stub_llvm_rint_f32_begin:
    frintx s0, s0
    ret
_lr_stub_llvm_rint_f32_end:

.globl _lr_stub_llvm_nearbyint_f64_begin
.globl _lr_stub_llvm_nearbyint_f64_end
_lr_stub_llvm_nearbyint_f64_begin:
    frinti d0, d0
    ret
_lr_stub_llvm_nearbyint_f64_end:

.globl _lr_stub_llvm_nearbyint_f32_begin
.globl _lr_stub_llvm_nearbyint_f32_end
_lr_stub_llvm_nearbyint_f32_begin:
    frinti s0, s0
    ret
_lr_stub_llvm_nearbyint_f32_end:

.globl _lr_stub_llvm_fma_f64_begin
.globl _lr_stub_llvm_fma_f64_end
_lr_stub_llvm_fma_f64_begin:
    fmadd d0, d0, d1, d2
    ret
_lr_stub_llvm_fma_f64_end:

.globl _lr_stub_llvm_fma_f32_begin
.globl _lr_stub_llvm_fma_f32_end
_lr_stub_llvm_fma_f32_begin:
    fmadd s0, s0, s1, s2
    ret
_lr_stub_llvm_fma_f32_end:

.globl _lr_stub_llvm_fmuladd_v2f32_begin
.globl _lr_stub_llvm_fmuladd_v2f32_end
_lr_stub_llvm_fmuladd_v2f32_begin:
    fmul v0.2s, v0.2s, v1.2s
    fadd v0.2s, v0.2s, v2.2s
    ret
_lr_stub_llvm_fmuladd_v2f32_end:

.globl _lr_stub_llvm_fmuladd_v4f32_begin
.globl _lr_stub_llvm_fmuladd_v4f32_end
_lr_stub_llvm_fmuladd_v4f32_begin:
    fmul v0.4s, v0.4s, v1.4s
    fadd v0.4s, v0.4s, v2.4s
    ret
_lr_stub_llvm_fmuladd_v4f32_end:

.globl _lr_stub_llvm_fmuladd_v2f64_begin
.globl _lr_stub_llvm_fmuladd_v2f64_end
_lr_stub_llvm_fmuladd_v2f64_begin:
    fmul v0.2d, v0.2d, v1.2d
    fadd v0.2d, v0.2d, v2.2d
    ret
_lr_stub_llvm_fmuladd_v2f64_end:

.globl _lr_stub_llvm_minnum_f64_begin
.globl _lr_stub_llvm_minnum_f64_end
_lr_stub_llvm_minnum_f64_begin:
    fminnm d0, d0, d1
    ret
_lr_stub_llvm_minnum_f64_end:

.globl _lr_stub_llvm_minnum_f32_begin
.globl _lr_stub_llvm_minnum_f32_end
_lr_stub_llvm_minnum_f32_begin:
    fminnm s0, s0, s1
    ret
_lr_stub_llvm_minnum_f32_end:

.globl _lr_stub_llvm_maxnum_f64_begin
.globl _lr_stub_llvm_maxnum_f64_end
_lr_stub_llvm_maxnum_f64_begin:
    fmaxnm d0, d0, d1
    ret
_lr_stub_llvm_maxnum_f64_end:

.globl _lr_stub_llvm_maxnum_f32_begin
.globl _lr_stub_llvm_maxnum_f32_end
_lr_stub_llvm_maxnum_f32_begin:
    fmaxnm s0, s0, s1
    ret
_lr_stub_llvm_maxnum_f32_end:

.globl _lr_stub_llvm_abs_i32_begin
.globl _lr_stub_llvm_abs_i32_end
_lr_stub_llvm_abs_i32_begin:
    cmp w0, #0
    cneg w0, w0, lt
    ret
_lr_stub_llvm_abs_i32_end:

.globl _lr_stub_llvm_abs_i8_begin
.globl _lr_stub_llvm_abs_i8_end
_lr_stub_llvm_abs_i8_begin:
    sxtb w0, w0
    cmp w0, #0
    cneg w0, w0, lt
    ret
_lr_stub_llvm_abs_i8_end:

.globl _lr_stub_llvm_abs_i16_begin
.globl _lr_stub_llvm_abs_i16_end
_lr_stub_llvm_abs_i16_begin:
    sxth w0, w0
    cmp w0, #0
    cneg w0, w0, lt
    ret
_lr_stub_llvm_abs_i16_end:

.globl _lr_stub_llvm_abs_i64_begin
.globl _lr_stub_llvm_abs_i64_end
_lr_stub_llvm_abs_i64_begin:
    cmp x0, #0
    cneg x0, x0, lt
    ret
_lr_stub_llvm_abs_i64_end:

.globl _lr_stub_llvm_assume_begin
.globl _lr_stub_llvm_assume_end
_lr_stub_llvm_assume_begin:
    ret
_lr_stub_llvm_assume_end:

.globl _lr_stub_llvm_trap_begin
.globl _lr_stub_llvm_trap_end
_lr_stub_llvm_trap_begin:
    brk #0
    ret
_lr_stub_llvm_trap_end:

.globl _lr_stub_llvm_exp2_f64_begin
.globl _lr_stub_llvm_exp2_f64_end
_lr_stub_llvm_exp2_f64_begin:
    adr x11, Lexp2_f64_ln2
    ldr d1, [x11]
    fmul d0, d0, d1

    fmov d2, #1.0
    fmov d3, #1.0
    mov x9, #1
    mov x10, #24
Lexp2_f64_loop:
    scvtf d4, x9
    fmul d2, d2, d0
    fdiv d2, d2, d4
    fadd d3, d3, d2
    add x9, x9, #1
    cmp x9, x10
    ble Lexp2_f64_loop
    fmov d0, d3
    ret
    .p2align 3
Lexp2_f64_ln2:
    .double 0.69314718055994530942
_lr_stub_llvm_exp2_f64_end:

.globl _lr_stub_llvm_exp2_f32_begin
.globl _lr_stub_llvm_exp2_f32_end
_lr_stub_llvm_exp2_f32_begin:
    fcvt d0, s0
    adr x11, Lexp2_f32_ln2
    ldr d1, [x11]
    fmul d0, d0, d1

    fmov d2, #1.0
    fmov d3, #1.0
    mov x9, #1
    mov x10, #20
Lexp2_f32_loop:
    scvtf d4, x9
    fmul d2, d2, d0
    fdiv d2, d2, d4
    fadd d3, d3, d2
    add x9, x9, #1
    cmp x9, x10
    ble Lexp2_f32_loop
    fcvt s0, d3
    ret
    .p2align 3
Lexp2_f32_ln2:
    .double 0.69314718055994530942
_lr_stub_llvm_exp2_f32_end:

.globl _lr_stub_llvm_exp10_f64_begin
.globl _lr_stub_llvm_exp10_f64_end
_lr_stub_llvm_exp10_f64_begin:
    adr x11, Lexp10_f64_ln10
    ldr d1, [x11]
    fmul d0, d0, d1

    fmov d2, #1.0
    fmov d3, #1.0
    mov x9, #1
    mov x10, #24
Lexp10_f64_loop:
    scvtf d4, x9
    fmul d2, d2, d0
    fdiv d2, d2, d4
    fadd d3, d3, d2
    add x9, x9, #1
    cmp x9, x10
    ble Lexp10_f64_loop
    fmov d0, d3
    ret
    .p2align 3
Lexp10_f64_ln10:
    .double 2.30258509299404568402
_lr_stub_llvm_exp10_f64_end:

.globl _lr_stub_llvm_exp10_f32_begin
.globl _lr_stub_llvm_exp10_f32_end
_lr_stub_llvm_exp10_f32_begin:
    fcvt d0, s0
    adr x11, Lexp10_f32_ln10
    ldr d1, [x11]
    fmul d0, d0, d1

    fmov d2, #1.0
    fmov d3, #1.0
    mov x9, #1
    mov x10, #20
Lexp10_f32_loop:
    scvtf d4, x9
    fmul d2, d2, d0
    fdiv d2, d2, d4
    fadd d3, d3, d2
    add x9, x9, #1
    cmp x9, x10
    ble Lexp10_f32_loop
    fcvt s0, d3
    ret
    .p2align 3
Lexp10_f32_ln10:
    .double 2.30258509299404568402
_lr_stub_llvm_exp10_f32_end:

.globl _lr_stub_llvm_log_f64_begin
.globl _lr_stub_llvm_log_f64_end
_lr_stub_llvm_log_f64_begin:
    fcmp d0, #0.0
    b.le Llog_f64_nonpos

    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d4, d2, d3
    fmul d5, d4, d4
    fmov d6, d4
    fmov d7, d4

    mov x9, #3
    mov x10, #39
Llog_f64_loop:
    fmul d6, d6, d5
    scvtf d8, x9
    fdiv d8, d6, d8
    fadd d7, d7, d8
    add x9, x9, #2
    cmp x9, x10
    ble Llog_f64_loop

    fmov d8, #2.0
    fmul d0, d7, d8
    ret
Llog_f64_nonpos:
    fmov d0, xzr
    ret
_lr_stub_llvm_log_f64_end:

.globl _lr_stub_llvm_log_f32_begin
.globl _lr_stub_llvm_log_f32_end
_lr_stub_llvm_log_f32_begin:
    fcvt d0, s0
    fcmp d0, #0.0
    b.le Llog_f32_nonpos

    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d4, d2, d3
    fmul d5, d4, d4
    fmov d6, d4
    fmov d7, d4

    mov x9, #3
    mov x10, #31
Llog_f32_loop:
    fmul d6, d6, d5
    scvtf d8, x9
    fdiv d8, d6, d8
    fadd d7, d7, d8
    add x9, x9, #2
    cmp x9, x10
    ble Llog_f32_loop

    fmov d8, #2.0
    fmul d0, d7, d8
    fcvt s0, d0
    ret
Llog_f32_nonpos:
    fmov s0, wzr
    ret
_lr_stub_llvm_log_f32_end:

.globl _lr_stub_llvm_log2_f64_begin
.globl _lr_stub_llvm_log2_f64_end
_lr_stub_llvm_log2_f64_begin:
    fcmp d0, #0.0
    b.le Llog2_f64_nonpos

    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d4, d2, d3
    fmul d5, d4, d4
    fmov d6, d4
    fmov d7, d4

    mov x9, #3
    mov x10, #39
Llog2_f64_loop:
    fmul d6, d6, d5
    scvtf d8, x9
    fdiv d8, d6, d8
    fadd d7, d7, d8
    add x9, x9, #2
    cmp x9, x10
    ble Llog2_f64_loop

    fmov d8, #2.0
    fmul d7, d7, d8
    adr x11, Llog2_f64_inv_ln2
    ldr d8, [x11]
    fmul d0, d7, d8
    ret
Llog2_f64_nonpos:
    fmov d0, xzr
    ret
    .p2align 3
Llog2_f64_inv_ln2:
    .double 1.44269504088896340736
_lr_stub_llvm_log2_f64_end:

.globl _lr_stub_llvm_log2_f32_begin
.globl _lr_stub_llvm_log2_f32_end
_lr_stub_llvm_log2_f32_begin:
    fcvt d0, s0
    fcmp d0, #0.0
    b.le Llog2_f32_nonpos

    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d4, d2, d3
    fmul d5, d4, d4
    fmov d6, d4
    fmov d7, d4

    mov x9, #3
    mov x10, #31
Llog2_f32_loop:
    fmul d6, d6, d5
    scvtf d8, x9
    fdiv d8, d6, d8
    fadd d7, d7, d8
    add x9, x9, #2
    cmp x9, x10
    ble Llog2_f32_loop

    fmov d8, #2.0
    fmul d7, d7, d8
    adr x11, Llog2_f32_inv_ln2
    ldr d8, [x11]
    fmul d0, d7, d8
    fcvt s0, d0
    ret
Llog2_f32_nonpos:
    fmov s0, wzr
    ret
    .p2align 3
Llog2_f32_inv_ln2:
    .double 1.44269504088896340736
_lr_stub_llvm_log2_f32_end:

.globl _lr_stub_llvm_log10_f64_begin
.globl _lr_stub_llvm_log10_f64_end
_lr_stub_llvm_log10_f64_begin:
    fcmp d0, #0.0
    b.le Llog10_f64_nonpos

    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d4, d2, d3
    fmul d5, d4, d4
    fmov d6, d4
    fmov d7, d4

    mov x9, #3
    mov x10, #39
Llog10_f64_loop:
    fmul d6, d6, d5
    scvtf d8, x9
    fdiv d8, d6, d8
    fadd d7, d7, d8
    add x9, x9, #2
    cmp x9, x10
    ble Llog10_f64_loop

    fmov d8, #2.0
    fmul d7, d7, d8
    adr x11, Llog10_f64_inv_ln10
    ldr d8, [x11]
    fmul d0, d7, d8
    ret
Llog10_f64_nonpos:
    fmov d0, xzr
    ret
    .p2align 3
Llog10_f64_inv_ln10:
    .double 0.43429448190325182765
_lr_stub_llvm_log10_f64_end:

.globl _lr_stub_llvm_log10_f32_begin
.globl _lr_stub_llvm_log10_f32_end
_lr_stub_llvm_log10_f32_begin:
    fcvt d0, s0
    fcmp d0, #0.0
    b.le Llog10_f32_nonpos

    fmov d1, #1.0
    fsub d2, d0, d1
    fadd d3, d0, d1
    fdiv d4, d2, d3
    fmul d5, d4, d4
    fmov d6, d4
    fmov d7, d4

    mov x9, #3
    mov x10, #31
Llog10_f32_loop:
    fmul d6, d6, d5
    scvtf d8, x9
    fdiv d8, d6, d8
    fadd d7, d7, d8
    add x9, x9, #2
    cmp x9, x10
    ble Llog10_f32_loop

    fmov d8, #2.0
    fmul d7, d7, d8
    adr x11, Llog10_f32_inv_ln10
    ldr d8, [x11]
    fmul d0, d7, d8
    fcvt s0, d0
    ret
Llog10_f32_nonpos:
    fmov s0, wzr
    ret
    .p2align 3
Llog10_f32_inv_ln10:
    .double 0.43429448190325182765
_lr_stub_llvm_log10_f32_end:

.globl _lr_stub_llvm_sin_f64_begin
.globl _lr_stub_llvm_sin_f64_end
_lr_stub_llvm_sin_f64_begin:
    adr x11, Lsin_f64_inv_two_pi
    ldr d10, [x11]
    fmul d11, d0, d10
    frintn d11, d11
    adr x11, Lsin_f64_two_pi
    ldr d10, [x11]
    fmul d12, d11, d10
    fsub d0, d0, d12

    fmul d1, d0, d0

    adr x11, Lsin_f64_c11
    ldr d2, [x11]
    fmul d2, d2, d1
    adr x11, Lsin_f64_c9
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lsin_f64_c7
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lsin_f64_c5
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lsin_f64_c3
    ldr d3, [x11]
    fadd d2, d2, d3

    fmul d2, d2, d1
    fmul d2, d2, d0
    fadd d0, d0, d2
    ret
    .p2align 3
Lsin_f64_inv_two_pi:
    .double 0.15915494309189533577
Lsin_f64_two_pi:
    .double 6.28318530717958647692
Lsin_f64_c3:
    .double -0.16666666666666666667
Lsin_f64_c5:
    .double 0.00833333333333333333
Lsin_f64_c7:
    .double -0.00019841269841269841
Lsin_f64_c9:
    .double 0.00000275573192239859
Lsin_f64_c11:
    .double -0.00000002505210838544
_lr_stub_llvm_sin_f64_end:

.globl _lr_stub_llvm_sin_f32_begin
.globl _lr_stub_llvm_sin_f32_end
_lr_stub_llvm_sin_f32_begin:
    fcvt d0, s0
    adr x11, Lsin_f32_inv_two_pi
    ldr d10, [x11]
    fmul d11, d0, d10
    frintn d11, d11
    adr x11, Lsin_f32_two_pi
    ldr d10, [x11]
    fmul d12, d11, d10
    fsub d0, d0, d12

    fmul d1, d0, d0

    adr x11, Lsin_f32_c11
    ldr d2, [x11]
    fmul d2, d2, d1
    adr x11, Lsin_f32_c9
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lsin_f32_c7
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lsin_f32_c5
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lsin_f32_c3
    ldr d3, [x11]
    fadd d2, d2, d3

    fmul d2, d2, d1
    fmul d2, d2, d0
    fadd d0, d0, d2
    fcvt s0, d0
    ret
    .p2align 3
Lsin_f32_inv_two_pi:
    .double 0.15915494309189533577
Lsin_f32_two_pi:
    .double 6.28318530717958647692
Lsin_f32_c3:
    .double -0.16666666666666666667
Lsin_f32_c5:
    .double 0.00833333333333333333
Lsin_f32_c7:
    .double -0.00019841269841269841
Lsin_f32_c9:
    .double 0.00000275573192239859
Lsin_f32_c11:
    .double -0.00000002505210838544
_lr_stub_llvm_sin_f32_end:

.globl _lr_stub_llvm_cos_f64_begin
.globl _lr_stub_llvm_cos_f64_end
_lr_stub_llvm_cos_f64_begin:
    adr x11, Lcos_f64_inv_two_pi
    ldr d10, [x11]
    fmul d11, d0, d10
    frintn d11, d11
    adr x11, Lcos_f64_two_pi
    ldr d10, [x11]
    fmul d12, d11, d10
    fsub d0, d0, d12

    fmul d1, d0, d0

    adr x11, Lcos_f64_c10
    ldr d2, [x11]
    fmul d2, d2, d1
    adr x11, Lcos_f64_c8
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lcos_f64_c6
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lcos_f64_c4
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lcos_f64_c2
    ldr d3, [x11]
    fadd d2, d2, d3

    fmul d2, d2, d1
    fmov d3, #1.0
    fadd d0, d3, d2
    ret
    .p2align 3
Lcos_f64_inv_two_pi:
    .double 0.15915494309189533577
Lcos_f64_two_pi:
    .double 6.28318530717958647692
Lcos_f64_c2:
    .double -0.5
Lcos_f64_c4:
    .double 0.04166666666666666667
Lcos_f64_c6:
    .double -0.00138888888888888889
Lcos_f64_c8:
    .double 0.00002480158730158730
Lcos_f64_c10:
    .double -0.00000027557319223986
_lr_stub_llvm_cos_f64_end:

.globl _lr_stub_llvm_cos_f32_begin
.globl _lr_stub_llvm_cos_f32_end
_lr_stub_llvm_cos_f32_begin:
    fcvt d0, s0
    adr x11, Lcos_f32_inv_two_pi
    ldr d10, [x11]
    fmul d11, d0, d10
    frintn d11, d11
    adr x11, Lcos_f32_two_pi
    ldr d10, [x11]
    fmul d12, d11, d10
    fsub d0, d0, d12

    fmul d1, d0, d0

    adr x11, Lcos_f32_c10
    ldr d2, [x11]
    fmul d2, d2, d1
    adr x11, Lcos_f32_c8
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lcos_f32_c6
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lcos_f32_c4
    ldr d3, [x11]
    fadd d2, d2, d3
    fmul d2, d2, d1
    adr x11, Lcos_f32_c2
    ldr d3, [x11]
    fadd d2, d2, d3

    fmul d2, d2, d1
    fmov d3, #1.0
    fadd d0, d3, d2
    fcvt s0, d0
    ret
    .p2align 3
Lcos_f32_inv_two_pi:
    .double 0.15915494309189533577
Lcos_f32_two_pi:
    .double 6.28318530717958647692
Lcos_f32_c2:
    .double -0.5
Lcos_f32_c4:
    .double 0.04166666666666666667
Lcos_f32_c6:
    .double -0.00138888888888888889
Lcos_f32_c8:
    .double 0.00002480158730158730
Lcos_f32_c10:
    .double -0.00000027557319223986
_lr_stub_llvm_cos_f32_end:

.globl _lr_stub_llvm_is_fpclass_f64_begin
.globl _lr_stub_llvm_is_fpclass_f64_end
_lr_stub_llvm_is_fpclass_f64_begin:
    fmov x1, d0
    lsr x2, x1, #63
    lsr x3, x1, #52
    and x3, x3, #0x7ff
    and x4, x1, #0x000fffffffffffff

    cmp x3, #0x7ff
    b.ne Lfpc64_not_special
    cbnz x4, Lfpc64_nan
    mov w5, #512
    cbz x2, Lfpc64_check
    mov w5, #4
    b Lfpc64_check
Lfpc64_nan:
    tbnz x4, #51, Lfpc64_qnan
    mov w5, #1
    b Lfpc64_check
Lfpc64_qnan:
    mov w5, #2
    b Lfpc64_check
Lfpc64_not_special:
    cbnz x3, Lfpc64_normal
    cbnz x4, Lfpc64_subnormal
    mov w5, #64
    cbz x2, Lfpc64_check
    mov w5, #32
    b Lfpc64_check
Lfpc64_subnormal:
    mov w5, #128
    cbz x2, Lfpc64_check
    mov w5, #16
    b Lfpc64_check
Lfpc64_normal:
    mov w5, #256
    cbz x2, Lfpc64_check
    mov w5, #8
Lfpc64_check:
    and w5, w5, w0
    cmp w5, #0
    cset w0, ne
    ret
_lr_stub_llvm_is_fpclass_f64_end:

.globl _lr_stub_llvm_is_fpclass_f32_begin
.globl _lr_stub_llvm_is_fpclass_f32_end
_lr_stub_llvm_is_fpclass_f32_begin:
    fmov w1, s0
    lsr w2, w1, #31
    lsr w3, w1, #23
    and w3, w3, #0xff
    and w4, w1, #0x7fffff

    cmp w3, #0xff
    b.ne Lfpc32_not_special
    cbnz w4, Lfpc32_nan
    mov w5, #512
    cbz w2, Lfpc32_check
    mov w5, #4
    b Lfpc32_check
Lfpc32_nan:
    tbnz w4, #22, Lfpc32_qnan
    mov w5, #1
    b Lfpc32_check
Lfpc32_qnan:
    mov w5, #2
    b Lfpc32_check
Lfpc32_not_special:
    cbnz w3, Lfpc32_normal
    cbnz w4, Lfpc32_subnormal
    mov w5, #64
    cbz w2, Lfpc32_check
    mov w5, #32
    b Lfpc32_check
Lfpc32_subnormal:
    mov w5, #128
    cbz w2, Lfpc32_check
    mov w5, #16
    b Lfpc32_check
Lfpc32_normal:
    mov w5, #256
    cbz w2, Lfpc32_check
    mov w5, #8
Lfpc32_check:
    and w5, w5, w0
    cmp w5, #0
    cset w0, ne
    ret
_lr_stub_llvm_is_fpclass_f32_end:

.globl _lr_stub_llvm_smax_i32_begin
.globl _lr_stub_llvm_smax_i32_end
_lr_stub_llvm_smax_i32_begin:
    cmp w0, w1
    csel w0, w0, w1, ge
    ret
_lr_stub_llvm_smax_i32_end:

.globl _lr_stub_llvm_smin_i32_begin
.globl _lr_stub_llvm_smin_i32_end
_lr_stub_llvm_smin_i32_begin:
    cmp w0, w1
    csel w0, w0, w1, le
    ret
_lr_stub_llvm_smin_i32_end:

.globl _lr_stub_llvm_smax_i64_begin
.globl _lr_stub_llvm_smax_i64_end
_lr_stub_llvm_smax_i64_begin:
    cmp x0, x1
    csel x0, x0, x1, ge
    ret
_lr_stub_llvm_smax_i64_end:

.globl _lr_stub_llvm_smin_i64_begin
.globl _lr_stub_llvm_smin_i64_end
_lr_stub_llvm_smin_i64_begin:
    cmp x0, x1
    csel x0, x0, x1, le
    ret
_lr_stub_llvm_smin_i64_end:

.globl _lr_stub_llvm_umax_i32_begin
.globl _lr_stub_llvm_umax_i32_end
_lr_stub_llvm_umax_i32_begin:
    cmp w0, w1
    csel w0, w0, w1, hs
    ret
_lr_stub_llvm_umax_i32_end:

.globl _lr_stub_llvm_umin_i32_begin
.globl _lr_stub_llvm_umin_i32_end
_lr_stub_llvm_umin_i32_begin:
    cmp w0, w1
    csel w0, w0, w1, ls
    ret
_lr_stub_llvm_umin_i32_end:

.globl _lr_stub_llvm_umax_i64_begin
.globl _lr_stub_llvm_umax_i64_end
_lr_stub_llvm_umax_i64_begin:
    cmp x0, x1
    csel x0, x0, x1, hs
    ret
_lr_stub_llvm_umax_i64_end:

.globl _lr_stub_llvm_umin_i64_begin
.globl _lr_stub_llvm_umin_i64_end
_lr_stub_llvm_umin_i64_begin:
    cmp x0, x1
    csel x0, x0, x1, ls
    ret
_lr_stub_llvm_umin_i64_end:

.subsections_via_symbols
