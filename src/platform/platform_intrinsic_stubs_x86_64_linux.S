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
