.text
.intel_syntax noprefix

.globl lr_stub_llvm_fabs_f32_begin
.globl lr_stub_llvm_fabs_f32_end
lr_stub_llvm_fabs_f32_begin:
    movd eax, xmm0
    and eax, 0x7fffffff
    movd xmm0, eax
    ret
lr_stub_llvm_fabs_f32_end:

.globl lr_stub_llvm_fabs_v2f32_begin
.globl lr_stub_llvm_fabs_v2f32_end
lr_stub_llvm_fabs_v2f32_begin:
    pcmpeqd xmm1, xmm1
    psrld xmm1, 1
    andps xmm0, xmm1
    ret
lr_stub_llvm_fabs_v2f32_end:

.globl lr_stub_llvm_fabs_v2f64_begin
.globl lr_stub_llvm_fabs_v2f64_end
lr_stub_llvm_fabs_v2f64_begin:
    pcmpeqd xmm1, xmm1
    psrlq xmm1, 1
    andpd xmm0, xmm1
    ret
lr_stub_llvm_fabs_v2f64_end:

.globl lr_stub_llvm_lifetime_start_begin
.globl lr_stub_llvm_lifetime_start_end
lr_stub_llvm_lifetime_start_begin:
    ret
lr_stub_llvm_lifetime_start_end:

.globl lr_stub_llvm_lifetime_end_begin
.globl lr_stub_llvm_lifetime_end_end
lr_stub_llvm_lifetime_end_begin:
    ret
lr_stub_llvm_lifetime_end_end:

.globl lr_stub_llvm_fabs_f64_begin
.globl lr_stub_llvm_fabs_f64_end
lr_stub_llvm_fabs_f64_begin:
    movq rax, xmm0
    movabs rcx, 0x7fffffffffffffff
    and rax, rcx
    movq xmm0, rax
    ret
lr_stub_llvm_fabs_f64_end:

.globl lr_stub_llvm_sqrt_f32_begin
.globl lr_stub_llvm_sqrt_f32_end
lr_stub_llvm_sqrt_f32_begin:
    sqrtss xmm0, xmm0
    ret
lr_stub_llvm_sqrt_f32_end:

.globl lr_stub_llvm_sqrt_f64_begin
.globl lr_stub_llvm_sqrt_f64_end
lr_stub_llvm_sqrt_f64_begin:
    sqrtsd xmm0, xmm0
    ret
lr_stub_llvm_sqrt_f64_end:

.globl lr_stub_llvm_copysign_f32_begin
.globl lr_stub_llvm_copysign_f32_end
lr_stub_llvm_copysign_f32_begin:
    movd eax, xmm0
    movd edx, xmm1
    and eax, 0x7fffffff
    and edx, 0x80000000
    or eax, edx
    movd xmm0, eax
    ret
lr_stub_llvm_copysign_f32_end:

.globl lr_stub_llvm_copysign_f64_begin
.globl lr_stub_llvm_copysign_f64_end
lr_stub_llvm_copysign_f64_begin:
    movq rax, xmm0
    movq rdx, xmm1
    movabs rcx, 0x7fffffffffffffff
    and rax, rcx
    movabs rcx, 0x8000000000000000
    and rdx, rcx
    or rax, rdx
    movq xmm0, rax
    ret
lr_stub_llvm_copysign_f64_end:

.globl lr_stub_llvm_exp_f64_begin
.globl lr_stub_llvm_exp_f64_end
lr_stub_llvm_exp_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fldl2e
    fmulp st(1), st(0)
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_exp_f64_end:

.globl lr_stub_llvm_exp_f32_begin
.globl lr_stub_llvm_exp_f32_end
lr_stub_llvm_exp_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fldl2e
    fmulp st(1), st(0)
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_exp_f32_end:

.globl lr_stub_llvm_pow_f64_begin
.globl lr_stub_llvm_pow_f64_end
lr_stub_llvm_pow_f64_begin:
    sub rsp, 16
    movsd qword ptr [rsp], xmm0
    movsd qword ptr [rsp + 8], xmm1
    fld qword ptr [rsp + 8]
    fld qword ptr [rsp]
    fyl2x
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 16
    ret
lr_stub_llvm_pow_f64_end:

.globl lr_stub_llvm_pow_f32_begin
.globl lr_stub_llvm_pow_f32_end
lr_stub_llvm_pow_f32_begin:
    sub rsp, 16
    cvtss2sd xmm0, xmm0
    cvtss2sd xmm1, xmm1
    movsd qword ptr [rsp], xmm0
    movsd qword ptr [rsp + 8], xmm1
    fld qword ptr [rsp + 8]
    fld qword ptr [rsp]
    fyl2x
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 16
    ret
lr_stub_llvm_pow_f32_end:

.globl lr_stub_llvm_powi_f32_i64_begin
.globl lr_stub_llvm_powi_f32_i64_end
lr_stub_llvm_powi_f32_i64_begin:
    mov eax, 0x3f800000
    movd xmm1, eax
    mov rax, rdi
    test rax, rax
    jge .Lpowi_f32_i64_loop_entry
    neg rax
    mov eax, 0x3f800000
    movd xmm2, eax
    divss xmm2, xmm0
    movaps xmm0, xmm2
.Lpowi_f32_i64_loop_entry:
    test rax, rax
    je .Lpowi_f32_i64_done
.Lpowi_f32_i64_loop:
    test rax, 1
    je .Lpowi_f32_i64_skip_mul
    mulss xmm1, xmm0
.Lpowi_f32_i64_skip_mul:
    mulss xmm0, xmm0
    shr rax, 1
    jne .Lpowi_f32_i64_loop
.Lpowi_f32_i64_done:
    movaps xmm0, xmm1
    ret
lr_stub_llvm_powi_f32_i64_end:

.globl lr_stub_llvm_powi_f64_i64_begin
.globl lr_stub_llvm_powi_f64_i64_end
lr_stub_llvm_powi_f64_i64_begin:
    movabs rax, 0x3ff0000000000000
    movq xmm1, rax
    mov rax, rdi
    test rax, rax
    jge .Lpowi_f64_i64_loop_entry
    neg rax
    movabs rdx, 0x3ff0000000000000
    movq xmm2, rdx
    divsd xmm2, xmm0
    movapd xmm0, xmm2
.Lpowi_f64_i64_loop_entry:
    test rax, rax
    je .Lpowi_f64_i64_done
.Lpowi_f64_i64_loop:
    test rax, 1
    je .Lpowi_f64_i64_skip_mul
    mulsd xmm1, xmm0
.Lpowi_f64_i64_skip_mul:
    mulsd xmm0, xmm0
    shr rax, 1
    jne .Lpowi_f64_i64_loop
.Lpowi_f64_i64_done:
    movapd xmm0, xmm1
    ret
lr_stub_llvm_powi_f64_i64_end:

.globl lr_stub_llvm_powi_f32_i32_begin
.globl lr_stub_llvm_powi_f32_i32_end
lr_stub_llvm_powi_f32_i32_begin:
    mov eax, 0x3f800000
    movd xmm1, eax
    movsxd rax, edi
    test rax, rax
    jge .Lpowi_f32_i32_loop_entry
    neg rax
    mov eax, 0x3f800000
    movd xmm2, eax
    divss xmm2, xmm0
    movaps xmm0, xmm2
.Lpowi_f32_i32_loop_entry:
    test rax, rax
    je .Lpowi_f32_i32_done
.Lpowi_f32_i32_loop:
    test rax, 1
    je .Lpowi_f32_i32_skip_mul
    mulss xmm1, xmm0
.Lpowi_f32_i32_skip_mul:
    mulss xmm0, xmm0
    shr rax, 1
    jne .Lpowi_f32_i32_loop
.Lpowi_f32_i32_done:
    movaps xmm0, xmm1
    ret
lr_stub_llvm_powi_f32_i32_end:

.globl lr_stub_llvm_powi_f64_i32_begin
.globl lr_stub_llvm_powi_f64_i32_end
lr_stub_llvm_powi_f64_i32_begin:
    movabs rax, 0x3ff0000000000000
    movq xmm1, rax
    movsxd rax, edi
    test rax, rax
    jge .Lpowi_f64_i32_loop_entry
    neg rax
    movabs rdx, 0x3ff0000000000000
    movq xmm2, rdx
    divsd xmm2, xmm0
    movapd xmm0, xmm2
.Lpowi_f64_i32_loop_entry:
    test rax, rax
    je .Lpowi_f64_i32_done
.Lpowi_f64_i32_loop:
    test rax, 1
    je .Lpowi_f64_i32_skip_mul
    mulsd xmm1, xmm0
.Lpowi_f64_i32_skip_mul:
    mulsd xmm0, xmm0
    shr rax, 1
    jne .Lpowi_f64_i32_loop
.Lpowi_f64_i32_done:
    movapd xmm0, xmm1
    ret
lr_stub_llvm_powi_f64_i32_end:

.globl lr_stub_llvm_memset_i32_begin
.globl lr_stub_llvm_memset_i32_end
lr_stub_llvm_memset_i32_begin:
    test edx, edx
    jle .Lmemset_i32_done
    mov ecx, edx
    mov al, sil
    rep stosb
.Lmemset_i32_done:
    ret
lr_stub_llvm_memset_i32_end:

.globl lr_stub_llvm_memset_i64_begin
.globl lr_stub_llvm_memset_i64_end
lr_stub_llvm_memset_i64_begin:
    test rdx, rdx
    jle .Lmemset_i64_done
    mov rcx, rdx
    mov al, sil
    rep stosb
.Lmemset_i64_done:
    ret
lr_stub_llvm_memset_i64_end:

.globl lr_stub_llvm_memcpy_i32_begin
.globl lr_stub_llvm_memcpy_i32_end
lr_stub_llvm_memcpy_i32_begin:
    test edx, edx
    jle .Lmemcpy_i32_done
    mov ecx, edx
    rep movsb
.Lmemcpy_i32_done:
    ret
lr_stub_llvm_memcpy_i32_end:

.globl lr_stub_llvm_memcpy_i64_begin
.globl lr_stub_llvm_memcpy_i64_end
lr_stub_llvm_memcpy_i64_begin:
    test rdx, rdx
    jle .Lmemcpy_i64_done
    mov rcx, rdx
    rep movsb
.Lmemcpy_i64_done:
    ret
lr_stub_llvm_memcpy_i64_end:

.globl lr_stub_llvm_memmove_i32_begin
.globl lr_stub_llvm_memmove_i32_end
lr_stub_llvm_memmove_i32_begin:
    test edx, edx
    jle .Lmemmove_i32_done
    movsxd rdx, edx
    cmp rdi, rsi
    je .Lmemmove_i32_done
    jb .Lmemmove_i32_forward
    lea r8, [rsi + rdx]
    cmp rdi, r8
    jae .Lmemmove_i32_forward
    lea rsi, [rsi + rdx - 1]
    lea rdi, [rdi + rdx - 1]
    mov rcx, rdx
    std
    rep movsb
    cld
    ret
.Lmemmove_i32_forward:
    mov rcx, rdx
    rep movsb
.Lmemmove_i32_done:
    ret
lr_stub_llvm_memmove_i32_end:

.globl lr_stub_llvm_memmove_i64_begin
.globl lr_stub_llvm_memmove_i64_end
lr_stub_llvm_memmove_i64_begin:
    test rdx, rdx
    jle .Lmemmove_i64_done
    cmp rdi, rsi
    je .Lmemmove_i64_done
    jb .Lmemmove_i64_forward
    lea r8, [rsi + rdx]
    cmp rdi, r8
    jae .Lmemmove_i64_forward
    lea rsi, [rsi + rdx - 1]
    lea rdi, [rdi + rdx - 1]
    mov rcx, rdx
    std
    rep movsb
    cld
    ret
.Lmemmove_i64_forward:
    mov rcx, rdx
    rep movsb
.Lmemmove_i64_done:
    ret
lr_stub_llvm_memmove_i64_end:

.globl lr_stub_llvm_sin_f64_begin
.globl lr_stub_llvm_sin_f64_end
lr_stub_llvm_sin_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsin
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_sin_f64_end:

.globl lr_stub_llvm_sin_f32_begin
.globl lr_stub_llvm_sin_f32_end
lr_stub_llvm_sin_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fsin
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_sin_f32_end:

.globl lr_stub_llvm_cos_f64_begin
.globl lr_stub_llvm_cos_f64_end
lr_stub_llvm_cos_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fcos
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_cos_f64_end:

.globl lr_stub_llvm_cos_f32_begin
.globl lr_stub_llvm_cos_f32_end
lr_stub_llvm_cos_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fcos
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_cos_f32_end:

.globl lr_stub_llvm_log_f64_begin
.globl lr_stub_llvm_log_f64_end
lr_stub_llvm_log_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fldln2
    fld qword ptr [rsp]
    fyl2x
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_log_f64_end:

.globl lr_stub_llvm_log_f32_begin
.globl lr_stub_llvm_log_f32_end
lr_stub_llvm_log_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fldln2
    fld qword ptr [rsp]
    fyl2x
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_log_f32_end:

.globl lr_stub_llvm_log2_f64_begin
.globl lr_stub_llvm_log2_f64_end
lr_stub_llvm_log2_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld1
    fld qword ptr [rsp]
    fyl2x
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_log2_f64_end:

.globl lr_stub_llvm_log2_f32_begin
.globl lr_stub_llvm_log2_f32_end
lr_stub_llvm_log2_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fld1
    fld qword ptr [rsp]
    fyl2x
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_log2_f32_end:

.globl lr_stub_llvm_log10_f64_begin
.globl lr_stub_llvm_log10_f64_end
lr_stub_llvm_log10_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fldlg2
    fld qword ptr [rsp]
    fyl2x
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_log10_f64_end:

.globl lr_stub_llvm_log10_f32_begin
.globl lr_stub_llvm_log10_f32_end
lr_stub_llvm_log10_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fldlg2
    fld qword ptr [rsp]
    fyl2x
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_log10_f32_end:

.globl lr_stub_llvm_exp2_f64_begin
.globl lr_stub_llvm_exp2_f64_end
lr_stub_llvm_exp2_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_exp2_f64_end:

.globl lr_stub_llvm_exp2_f32_begin
.globl lr_stub_llvm_exp2_f32_end
lr_stub_llvm_exp2_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_exp2_f32_end:

.globl lr_stub_llvm_exp10_f64_begin
.globl lr_stub_llvm_exp10_f64_end
lr_stub_llvm_exp10_f64_begin:
    sub rsp, 8
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fldl2t
    fmulp st(1), st(0)
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    add rsp, 8
    ret
lr_stub_llvm_exp10_f64_end:

.globl lr_stub_llvm_exp10_f32_begin
.globl lr_stub_llvm_exp10_f32_end
lr_stub_llvm_exp10_f32_begin:
    sub rsp, 8
    cvtss2sd xmm0, xmm0
    movsd qword ptr [rsp], xmm0
    fld qword ptr [rsp]
    fldl2t
    fmulp st(1), st(0)
    fld st(0)
    frndint
    fsub st(1), st(0)
    fxch st(1)
    f2xm1
    fld1
    faddp st(1), st(0)
    fscale
    fstp st(1)
    fstp qword ptr [rsp]
    movsd xmm0, qword ptr [rsp]
    cvtsd2ss xmm0, xmm0
    add rsp, 8
    ret
lr_stub_llvm_exp10_f32_end:

.globl lr_stub_llvm_floor_f64_begin
.globl lr_stub_llvm_floor_f64_end
lr_stub_llvm_floor_f64_begin:
    roundsd xmm0, xmm0, 0x09
    ret
lr_stub_llvm_floor_f64_end:

.globl lr_stub_llvm_floor_f32_begin
.globl lr_stub_llvm_floor_f32_end
lr_stub_llvm_floor_f32_begin:
    roundss xmm0, xmm0, 0x09
    ret
lr_stub_llvm_floor_f32_end:

.globl lr_stub_llvm_ceil_f64_begin
.globl lr_stub_llvm_ceil_f64_end
lr_stub_llvm_ceil_f64_begin:
    roundsd xmm0, xmm0, 0x0A
    ret
lr_stub_llvm_ceil_f64_end:

.globl lr_stub_llvm_ceil_f32_begin
.globl lr_stub_llvm_ceil_f32_end
lr_stub_llvm_ceil_f32_begin:
    roundss xmm0, xmm0, 0x0A
    ret
lr_stub_llvm_ceil_f32_end:

.globl lr_stub_llvm_trunc_f64_begin
.globl lr_stub_llvm_trunc_f64_end
lr_stub_llvm_trunc_f64_begin:
    roundsd xmm0, xmm0, 0x0B
    ret
lr_stub_llvm_trunc_f64_end:

.globl lr_stub_llvm_trunc_f32_begin
.globl lr_stub_llvm_trunc_f32_end
lr_stub_llvm_trunc_f32_begin:
    roundss xmm0, xmm0, 0x0B
    ret
lr_stub_llvm_trunc_f32_end:

.globl lr_stub_llvm_round_f64_begin
.globl lr_stub_llvm_round_f64_end
lr_stub_llvm_round_f64_begin:
    movapd xmm1, xmm0
    movabs rax, 0x3FE0000000000000
    movq xmm2, rax
    movabs rax, 0x8000000000000000
    movq xmm3, rax
    andpd xmm3, xmm1
    orpd xmm2, xmm3
    addsd xmm0, xmm2
    roundsd xmm0, xmm0, 0x0B
    ret
lr_stub_llvm_round_f64_end:

.globl lr_stub_llvm_round_f32_begin
.globl lr_stub_llvm_round_f32_end
lr_stub_llvm_round_f32_begin:
    movaps xmm1, xmm0
    mov eax, 0x3F000000
    movd xmm2, eax
    mov eax, 0x80000000
    movd xmm3, eax
    andps xmm3, xmm1
    orps xmm2, xmm3
    addss xmm0, xmm2
    roundss xmm0, xmm0, 0x0B
    ret
lr_stub_llvm_round_f32_end:

.globl lr_stub_llvm_rint_f64_begin
.globl lr_stub_llvm_rint_f64_end
lr_stub_llvm_rint_f64_begin:
    roundsd xmm0, xmm0, 0x04
    ret
lr_stub_llvm_rint_f64_end:

.globl lr_stub_llvm_rint_f32_begin
.globl lr_stub_llvm_rint_f32_end
lr_stub_llvm_rint_f32_begin:
    roundss xmm0, xmm0, 0x04
    ret
lr_stub_llvm_rint_f32_end:

.globl lr_stub_llvm_nearbyint_f64_begin
.globl lr_stub_llvm_nearbyint_f64_end
lr_stub_llvm_nearbyint_f64_begin:
    roundsd xmm0, xmm0, 0x0C
    ret
lr_stub_llvm_nearbyint_f64_end:

.globl lr_stub_llvm_nearbyint_f32_begin
.globl lr_stub_llvm_nearbyint_f32_end
lr_stub_llvm_nearbyint_f32_begin:
    roundss xmm0, xmm0, 0x0C
    ret
lr_stub_llvm_nearbyint_f32_end:

.globl lr_stub_llvm_fma_f64_begin
.globl lr_stub_llvm_fma_f64_end
lr_stub_llvm_fma_f64_begin:
    vfmadd213sd xmm0, xmm1, xmm2
    ret
lr_stub_llvm_fma_f64_end:

.globl lr_stub_llvm_fma_f32_begin
.globl lr_stub_llvm_fma_f32_end
lr_stub_llvm_fma_f32_begin:
    vfmadd213ss xmm0, xmm1, xmm2
    ret
lr_stub_llvm_fma_f32_end:

.globl lr_stub_llvm_fmuladd_v2f32_begin
.globl lr_stub_llvm_fmuladd_v2f32_end
lr_stub_llvm_fmuladd_v2f32_begin:
    mulps xmm0, xmm1
    addps xmm0, xmm2
    ret
lr_stub_llvm_fmuladd_v2f32_end:

.globl lr_stub_llvm_fmuladd_v4f32_begin
.globl lr_stub_llvm_fmuladd_v4f32_end
lr_stub_llvm_fmuladd_v4f32_begin:
    mulps xmm0, xmm1
    addps xmm0, xmm2
    ret
lr_stub_llvm_fmuladd_v4f32_end:

.globl lr_stub_llvm_fmuladd_v2f64_begin
.globl lr_stub_llvm_fmuladd_v2f64_end
lr_stub_llvm_fmuladd_v2f64_begin:
    mulpd xmm0, xmm1
    addpd xmm0, xmm2
    ret
lr_stub_llvm_fmuladd_v2f64_end:

.globl lr_stub_llvm_minnum_f64_begin
.globl lr_stub_llvm_minnum_f64_end
lr_stub_llvm_minnum_f64_begin:
    ucomisd xmm1, xmm1
    jp .Lminnum_f64_ret
    minsd xmm0, xmm1
.Lminnum_f64_ret:
    ret
lr_stub_llvm_minnum_f64_end:

.globl lr_stub_llvm_minnum_f32_begin
.globl lr_stub_llvm_minnum_f32_end
lr_stub_llvm_minnum_f32_begin:
    ucomiss xmm1, xmm1
    jp .Lminnum_f32_ret
    minss xmm0, xmm1
.Lminnum_f32_ret:
    ret
lr_stub_llvm_minnum_f32_end:

.globl lr_stub_llvm_maxnum_f64_begin
.globl lr_stub_llvm_maxnum_f64_end
lr_stub_llvm_maxnum_f64_begin:
    ucomisd xmm1, xmm1
    jp .Lmaxnum_f64_ret
    maxsd xmm0, xmm1
.Lmaxnum_f64_ret:
    ret
lr_stub_llvm_maxnum_f64_end:

.globl lr_stub_llvm_maxnum_f32_begin
.globl lr_stub_llvm_maxnum_f32_end
lr_stub_llvm_maxnum_f32_begin:
    ucomiss xmm1, xmm1
    jp .Lmaxnum_f32_ret
    maxss xmm0, xmm1
.Lmaxnum_f32_ret:
    ret
lr_stub_llvm_maxnum_f32_end:

.globl lr_stub_llvm_abs_i32_begin
.globl lr_stub_llvm_abs_i32_end
lr_stub_llvm_abs_i32_begin:
    mov eax, edi
    neg eax
    cmovl eax, edi
    ret
lr_stub_llvm_abs_i32_end:

.globl lr_stub_llvm_abs_i8_begin
.globl lr_stub_llvm_abs_i8_end
lr_stub_llvm_abs_i8_begin:
    mov al, dil
    test al, al
    jns .Labs_i8_done
    neg al
.Labs_i8_done:
    ret
lr_stub_llvm_abs_i8_end:

.globl lr_stub_llvm_abs_i16_begin
.globl lr_stub_llvm_abs_i16_end
lr_stub_llvm_abs_i16_begin:
    mov ax, di
    test ax, ax
    jns .Labs_i16_done
    neg ax
.Labs_i16_done:
    ret
lr_stub_llvm_abs_i16_end:

.globl lr_stub_llvm_abs_i64_begin
.globl lr_stub_llvm_abs_i64_end
lr_stub_llvm_abs_i64_begin:
    mov rax, rdi
    neg rax
    cmovl rax, rdi
    ret
lr_stub_llvm_abs_i64_end:

.globl lr_stub_llvm_assume_begin
.globl lr_stub_llvm_assume_end
lr_stub_llvm_assume_begin:
    ret
lr_stub_llvm_assume_end:

.globl lr_stub_llvm_trap_begin
.globl lr_stub_llvm_trap_end
lr_stub_llvm_trap_begin:
    ud2
    ret
lr_stub_llvm_trap_end:

.globl lr_stub_llvm_is_fpclass_f64_begin
.globl lr_stub_llvm_is_fpclass_f64_end
lr_stub_llvm_is_fpclass_f64_begin:
    movq rax, xmm0
    mov ecx, eax
    shr ecx, 31
    mov rdx, rax
    shr rdx, 52
    and edx, 0x7FF
    movabs r8, 0x000FFFFFFFFFFFFF
    and rax, r8
    cmp edx, 0x7FF
    jne .Lfpc64_not_special
    test rax, rax
    jnz .Lfpc64_nan
    mov eax, 512
    test ecx, 1
    jz .Lfpc64_check
    mov eax, 4
    jmp .Lfpc64_check
.Lfpc64_nan:
    bt rax, 51
    jc .Lfpc64_qnan
    mov eax, 1
    jmp .Lfpc64_check
.Lfpc64_qnan:
    mov eax, 2
    jmp .Lfpc64_check
.Lfpc64_not_special:
    test edx, edx
    jnz .Lfpc64_normal
    test rax, rax
    jnz .Lfpc64_subnormal
    mov eax, 64
    test ecx, 1
    jz .Lfpc64_check
    mov eax, 32
    jmp .Lfpc64_check
.Lfpc64_subnormal:
    mov eax, 128
    test ecx, 1
    jz .Lfpc64_check
    mov eax, 16
    jmp .Lfpc64_check
.Lfpc64_normal:
    mov eax, 256
    test ecx, 1
    jz .Lfpc64_check
    mov eax, 8
.Lfpc64_check:
    and eax, edi
    test eax, eax
    setnz al
    movzx eax, al
    ret
lr_stub_llvm_is_fpclass_f64_end:

.globl lr_stub_llvm_is_fpclass_f32_begin
.globl lr_stub_llvm_is_fpclass_f32_end
lr_stub_llvm_is_fpclass_f32_begin:
    movd eax, xmm0
    mov ecx, eax
    shr ecx, 31
    mov edx, eax
    shr edx, 23
    and edx, 0xFF
    and eax, 0x7FFFFF
    cmp edx, 0xFF
    jne .Lfpc32_not_special
    test eax, eax
    jnz .Lfpc32_nan
    mov eax, 512
    test ecx, 1
    jz .Lfpc32_check
    mov eax, 4
    jmp .Lfpc32_check
.Lfpc32_nan:
    bt eax, 22
    jc .Lfpc32_qnan
    mov eax, 1
    jmp .Lfpc32_check
.Lfpc32_qnan:
    mov eax, 2
    jmp .Lfpc32_check
.Lfpc32_not_special:
    test edx, edx
    jnz .Lfpc32_normal
    test eax, eax
    jnz .Lfpc32_subnormal
    mov eax, 64
    test ecx, 1
    jz .Lfpc32_check
    mov eax, 32
    jmp .Lfpc32_check
.Lfpc32_subnormal:
    mov eax, 128
    test ecx, 1
    jz .Lfpc32_check
    mov eax, 16
    jmp .Lfpc32_check
.Lfpc32_normal:
    mov eax, 256
    test ecx, 1
    jz .Lfpc32_check
    mov eax, 8
.Lfpc32_check:
    and eax, edi
    test eax, eax
    setnz al
    movzx eax, al
    ret
lr_stub_llvm_is_fpclass_f32_end:

.globl lr_stub_llvm_smax_i32_begin
.globl lr_stub_llvm_smax_i32_end
lr_stub_llvm_smax_i32_begin:
    mov eax, edi
    cmp edi, esi
    cmovl eax, esi
    ret
lr_stub_llvm_smax_i32_end:

.globl lr_stub_llvm_smin_i32_begin
.globl lr_stub_llvm_smin_i32_end
lr_stub_llvm_smin_i32_begin:
    mov eax, edi
    cmp edi, esi
    cmovg eax, esi
    ret
lr_stub_llvm_smin_i32_end:

.globl lr_stub_llvm_smax_i64_begin
.globl lr_stub_llvm_smax_i64_end
lr_stub_llvm_smax_i64_begin:
    mov rax, rdi
    cmp rdi, rsi
    cmovl rax, rsi
    ret
lr_stub_llvm_smax_i64_end:

.globl lr_stub_llvm_smin_i64_begin
.globl lr_stub_llvm_smin_i64_end
lr_stub_llvm_smin_i64_begin:
    mov rax, rdi
    cmp rdi, rsi
    cmovg rax, rsi
    ret
lr_stub_llvm_smin_i64_end:

.globl lr_stub_llvm_umax_i32_begin
.globl lr_stub_llvm_umax_i32_end
lr_stub_llvm_umax_i32_begin:
    mov eax, edi
    cmp edi, esi
    cmovb eax, esi
    ret
lr_stub_llvm_umax_i32_end:

.globl lr_stub_llvm_umin_i32_begin
.globl lr_stub_llvm_umin_i32_end
lr_stub_llvm_umin_i32_begin:
    mov eax, edi
    cmp edi, esi
    cmova eax, esi
    ret
lr_stub_llvm_umin_i32_end:

.globl lr_stub_llvm_umax_i64_begin
.globl lr_stub_llvm_umax_i64_end
lr_stub_llvm_umax_i64_begin:
    mov rax, rdi
    cmp rdi, rsi
    cmovb rax, rsi
    ret
lr_stub_llvm_umax_i64_end:

.globl lr_stub_llvm_umin_i64_begin
.globl lr_stub_llvm_umin_i64_end
lr_stub_llvm_umin_i64_begin:
    mov rax, rdi
    cmp rdi, rsi
    cmova rax, rsi
    ret
lr_stub_llvm_umin_i64_end:
