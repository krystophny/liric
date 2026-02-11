.text

.globl lr_stub_llvm_fabs_f32_begin
.globl lr_stub_llvm_fabs_f32_end
lr_stub_llvm_fabs_f32_begin:
    fabs s0, s0
    ret
lr_stub_llvm_fabs_f32_end:

.globl lr_stub_llvm_fabs_f64_begin
.globl lr_stub_llvm_fabs_f64_end
lr_stub_llvm_fabs_f64_begin:
    fabs d0, d0
    ret
lr_stub_llvm_fabs_f64_end:

.globl lr_stub_llvm_sqrt_f32_begin
.globl lr_stub_llvm_sqrt_f32_end
lr_stub_llvm_sqrt_f32_begin:
    fsqrt s0, s0
    ret
lr_stub_llvm_sqrt_f32_end:

.globl lr_stub_llvm_sqrt_f64_begin
.globl lr_stub_llvm_sqrt_f64_end
lr_stub_llvm_sqrt_f64_begin:
    fsqrt d0, d0
    ret
lr_stub_llvm_sqrt_f64_end:

.globl lr_stub_llvm_copysign_f32_begin
.globl lr_stub_llvm_copysign_f32_end
lr_stub_llvm_copysign_f32_begin:
    fmov w2, s0
    fmov w3, s1
    and w2, w2, #0x7fffffff
    and w3, w3, #0x80000000
    orr w2, w2, w3
    fmov s0, w2
    ret
lr_stub_llvm_copysign_f32_end:

.globl lr_stub_llvm_copysign_f64_begin
.globl lr_stub_llvm_copysign_f64_end
lr_stub_llvm_copysign_f64_begin:
    fmov x2, d0
    fmov x3, d1
    and x2, x2, #0x7fffffffffffffff
    and x3, x3, #0x8000000000000000
    orr x2, x2, x3
    fmov d0, x2
    ret
lr_stub_llvm_copysign_f64_end:

.globl lr_stub_llvm_exp_f64_begin
.globl lr_stub_llvm_exp_f64_end
lr_stub_llvm_exp_f64_begin:
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
lr_stub_llvm_exp_f64_end:

.globl lr_stub_llvm_exp_f32_begin
.globl lr_stub_llvm_exp_f32_end
lr_stub_llvm_exp_f32_begin:
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
lr_stub_llvm_exp_f32_end:

.globl lr_stub_llvm_pow_f64_begin
.globl lr_stub_llvm_pow_f64_end
lr_stub_llvm_pow_f64_begin:
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
lr_stub_llvm_pow_f64_end:

.globl lr_stub_llvm_pow_f32_begin
.globl lr_stub_llvm_pow_f32_end
lr_stub_llvm_pow_f32_begin:
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
lr_stub_llvm_pow_f32_end:

.globl lr_stub_llvm_powi_f32_i32_begin
.globl lr_stub_llvm_powi_f32_i32_end
lr_stub_llvm_powi_f32_i32_begin:
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
lr_stub_llvm_powi_f32_i32_end:

.globl lr_stub_llvm_powi_f64_i32_begin
.globl lr_stub_llvm_powi_f64_i32_end
lr_stub_llvm_powi_f64_i32_begin:
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
lr_stub_llvm_powi_f64_i32_end:

.globl lr_stub_llvm_powi_f32_i64_begin
.globl lr_stub_llvm_powi_f32_i64_end
lr_stub_llvm_powi_f32_i64_begin:
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
lr_stub_llvm_powi_f32_i64_end:

.globl lr_stub_llvm_powi_f64_i64_begin
.globl lr_stub_llvm_powi_f64_i64_end
lr_stub_llvm_powi_f64_i64_begin:
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
lr_stub_llvm_powi_f64_i64_end:

.globl lr_stub_llvm_memset_i32_begin
.globl lr_stub_llvm_memset_i32_end
lr_stub_llvm_memset_i32_begin:
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
lr_stub_llvm_memset_i32_end:

.globl lr_stub_llvm_memset_i64_begin
.globl lr_stub_llvm_memset_i64_end
lr_stub_llvm_memset_i64_begin:
    cmp x2, #0
    ble 42f
    and w1, w1, #0xff
43:
    strb w1, [x0], #1
    subs x2, x2, #1
    b.ne 43b
42:
    ret
lr_stub_llvm_memset_i64_end:

.globl lr_stub_llvm_memcpy_i32_begin
.globl lr_stub_llvm_memcpy_i32_end
lr_stub_llvm_memcpy_i32_begin:
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
lr_stub_llvm_memcpy_i32_end:

.globl lr_stub_llvm_memcpy_i64_begin
.globl lr_stub_llvm_memcpy_i64_end
lr_stub_llvm_memcpy_i64_begin:
    cmp x2, #0
    ble 46f
47:
    ldrb w3, [x1], #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 47b
46:
    ret
lr_stub_llvm_memcpy_i64_end:

.globl lr_stub_llvm_memmove_i32_begin
.globl lr_stub_llvm_memmove_i32_end
lr_stub_llvm_memmove_i32_begin:
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
lr_stub_llvm_memmove_i32_end:

.globl lr_stub_llvm_memmove_i64_begin
.globl lr_stub_llvm_memmove_i64_end
lr_stub_llvm_memmove_i64_begin:
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
lr_stub_llvm_memmove_i64_end:
