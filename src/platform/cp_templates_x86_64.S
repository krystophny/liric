.text
.intel_syntax noprefix

/*
 * Copy-and-patch templates for x86_64.
 *
 * Each template is bracketed by _begin/_end labels so C code can
 * reference the byte range.  Sentinel displacements mark patch points:
 *   0x11111111 = src0 stack offset (rbp-relative)
 *   0x22222222 = src1 stack offset (rbp-relative)
 *   0x33333333 = dest stack offset (rbp-relative)
 *   0x44444444 = i32 immediate (frame size, constant, etc.)
 */

/* ---- prologue ---- */

.globl lr_cp_prologue_begin
.globl lr_cp_prologue_end
lr_cp_prologue_begin:
    push rbp
    mov rbp, rsp
    sub rsp, 0x44444444
lr_cp_prologue_end:

/* ---- ret i64 ---- */

.globl lr_cp_ret_i64_begin
.globl lr_cp_ret_i64_end
lr_cp_ret_i64_begin:
    mov rax, [rbp + 0x11111111]
    mov rsp, rbp
    pop rbp
    ret
lr_cp_ret_i64_end:

/* ---- ret i32 ---- */

.globl lr_cp_ret_i32_begin
.globl lr_cp_ret_i32_end
lr_cp_ret_i32_begin:
    mov eax, [rbp + 0x11111111]
    mov rsp, rbp
    pop rbp
    ret
lr_cp_ret_i32_end:

/* ---- ret void ---- */

.globl lr_cp_ret_void_begin
.globl lr_cp_ret_void_end
lr_cp_ret_void_begin:
    mov rsp, rbp
    pop rbp
    ret
lr_cp_ret_void_end:

/* ---- add i64 ---- */

.globl lr_cp_add_i64_begin
.globl lr_cp_add_i64_end
lr_cp_add_i64_begin:
    mov rax, [rbp + 0x11111111]
    add rax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], rax
lr_cp_add_i64_end:

/* ---- add i32 ---- */

.globl lr_cp_add_i32_begin
.globl lr_cp_add_i32_end
lr_cp_add_i32_begin:
    mov eax, [rbp + 0x11111111]
    add eax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], eax
lr_cp_add_i32_end:

/* ---- add+ret i64 supernode ---- */

.globl lr_cp_add_ret_i64_begin
.globl lr_cp_add_ret_i64_end
lr_cp_add_ret_i64_begin:
    mov rax, [rbp + 0x11111111]
    add rax, [rbp + 0x22222222]
    mov rsp, rbp
    pop rbp
    ret
lr_cp_add_ret_i64_end:

/* ---- add+ret i32 supernode ---- */

.globl lr_cp_add_ret_i32_begin
.globl lr_cp_add_ret_i32_end
lr_cp_add_ret_i32_begin:
    mov eax, [rbp + 0x11111111]
    add eax, [rbp + 0x22222222]
    mov rsp, rbp
    pop rbp
    ret
lr_cp_add_ret_i32_end:

/* ---- sub i64 ---- */

.globl lr_cp_sub_i64_begin
.globl lr_cp_sub_i64_end
lr_cp_sub_i64_begin:
    mov rax, [rbp + 0x11111111]
    sub rax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], rax
lr_cp_sub_i64_end:

/* ---- sub i32 ---- */

.globl lr_cp_sub_i32_begin
.globl lr_cp_sub_i32_end
lr_cp_sub_i32_begin:
    mov eax, [rbp + 0x11111111]
    sub eax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], eax
lr_cp_sub_i32_end:

/* ---- and i64 ---- */

.globl lr_cp_and_i64_begin
.globl lr_cp_and_i64_end
lr_cp_and_i64_begin:
    mov rax, [rbp + 0x11111111]
    and rax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], rax
lr_cp_and_i64_end:

/* ---- and i32 ---- */

.globl lr_cp_and_i32_begin
.globl lr_cp_and_i32_end
lr_cp_and_i32_begin:
    mov eax, [rbp + 0x11111111]
    and eax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], eax
lr_cp_and_i32_end:

/* ---- or i64 ---- */

.globl lr_cp_or_i64_begin
.globl lr_cp_or_i64_end
lr_cp_or_i64_begin:
    mov rax, [rbp + 0x11111111]
    or rax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], rax
lr_cp_or_i64_end:

/* ---- or i32 ---- */

.globl lr_cp_or_i32_begin
.globl lr_cp_or_i32_end
lr_cp_or_i32_begin:
    mov eax, [rbp + 0x11111111]
    or eax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], eax
lr_cp_or_i32_end:

/* ---- xor i64 ---- */

.globl lr_cp_xor_i64_begin
.globl lr_cp_xor_i64_end
lr_cp_xor_i64_begin:
    mov rax, [rbp + 0x11111111]
    xor rax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], rax
lr_cp_xor_i64_end:

/* ---- xor i32 ---- */

.globl lr_cp_xor_i32_begin
.globl lr_cp_xor_i32_end
lr_cp_xor_i32_begin:
    mov eax, [rbp + 0x11111111]
    xor eax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], eax
lr_cp_xor_i32_end:

/* ---- mul (imul) i64 ---- */

.globl lr_cp_mul_i64_begin
.globl lr_cp_mul_i64_end
lr_cp_mul_i64_begin:
    mov rax, [rbp + 0x11111111]
    imul rax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], rax
lr_cp_mul_i64_end:

/* ---- mul (imul) i32 ---- */

.globl lr_cp_mul_i32_begin
.globl lr_cp_mul_i32_end
lr_cp_mul_i32_begin:
    mov eax, [rbp + 0x11111111]
    imul eax, [rbp + 0x22222222]
    mov [rbp + 0x33333333], eax
lr_cp_mul_i32_end:

/* ---- sdiv i64 ---- */

.globl lr_cp_sdiv_i64_begin
.globl lr_cp_sdiv_i64_end
lr_cp_sdiv_i64_begin:
    mov rax, [rbp + 0x11111111]
    cqo
    mov rcx, [rbp + 0x22222222]
    idiv rcx
    mov [rbp + 0x33333333], rax
lr_cp_sdiv_i64_end:

/* ---- sdiv i32 ---- */

.globl lr_cp_sdiv_i32_begin
.globl lr_cp_sdiv_i32_end
lr_cp_sdiv_i32_begin:
    mov eax, [rbp + 0x11111111]
    cdq
    mov ecx, [rbp + 0x22222222]
    idiv ecx
    mov [rbp + 0x33333333], eax
lr_cp_sdiv_i32_end:

/* ---- srem i64 ---- */

.globl lr_cp_srem_i64_begin
.globl lr_cp_srem_i64_end
lr_cp_srem_i64_begin:
    mov rax, [rbp + 0x11111111]
    cqo
    mov rcx, [rbp + 0x22222222]
    idiv rcx
    mov [rbp + 0x33333333], rdx
lr_cp_srem_i64_end:

/* ---- srem i32 ---- */

.globl lr_cp_srem_i32_begin
.globl lr_cp_srem_i32_end
lr_cp_srem_i32_begin:
    mov eax, [rbp + 0x11111111]
    cdq
    mov ecx, [rbp + 0x22222222]
    idiv ecx
    mov [rbp + 0x33333333], edx
lr_cp_srem_i32_end:

/* ---- shl i64 ---- */

.globl lr_cp_shl_i64_begin
.globl lr_cp_shl_i64_end
lr_cp_shl_i64_begin:
    mov rax, [rbp + 0x11111111]
    mov ecx, [rbp + 0x22222222]
    shl rax, cl
    mov [rbp + 0x33333333], rax
lr_cp_shl_i64_end:

/* ---- shl i32 ---- */

.globl lr_cp_shl_i32_begin
.globl lr_cp_shl_i32_end
lr_cp_shl_i32_begin:
    mov eax, [rbp + 0x11111111]
    mov ecx, [rbp + 0x22222222]
    shl eax, cl
    mov [rbp + 0x33333333], eax
lr_cp_shl_i32_end:

/* ---- lshr i64 ---- */

.globl lr_cp_lshr_i64_begin
.globl lr_cp_lshr_i64_end
lr_cp_lshr_i64_begin:
    mov rax, [rbp + 0x11111111]
    mov ecx, [rbp + 0x22222222]
    shr rax, cl
    mov [rbp + 0x33333333], rax
lr_cp_lshr_i64_end:

/* ---- lshr i32 ---- */

.globl lr_cp_lshr_i32_begin
.globl lr_cp_lshr_i32_end
lr_cp_lshr_i32_begin:
    mov eax, [rbp + 0x11111111]
    mov ecx, [rbp + 0x22222222]
    shr eax, cl
    mov [rbp + 0x33333333], eax
lr_cp_lshr_i32_end:

/* ---- ashr i64 ---- */

.globl lr_cp_ashr_i64_begin
.globl lr_cp_ashr_i64_end
lr_cp_ashr_i64_begin:
    mov rax, [rbp + 0x11111111]
    mov ecx, [rbp + 0x22222222]
    sar rax, cl
    mov [rbp + 0x33333333], rax
lr_cp_ashr_i64_end:

/* ---- ashr i32 ---- */

.globl lr_cp_ashr_i32_begin
.globl lr_cp_ashr_i32_end
lr_cp_ashr_i32_begin:
    mov eax, [rbp + 0x11111111]
    mov ecx, [rbp + 0x22222222]
    sar eax, cl
    mov [rbp + 0x33333333], eax
lr_cp_ashr_i32_end:

/* ---- store param from register to stack slot ---- */
/* Used for parameter spilling.  Sentinel 0x33333333 = dest stack offset. */
/* These are referenced from C; the register is baked into each variant.  */

.globl lr_cp_store_param_rdi_begin
.globl lr_cp_store_param_rdi_end
lr_cp_store_param_rdi_begin:
    mov [rbp + 0x33333333], rdi
lr_cp_store_param_rdi_end:

.globl lr_cp_store_param_rsi_begin
.globl lr_cp_store_param_rsi_end
lr_cp_store_param_rsi_begin:
    mov [rbp + 0x33333333], rsi
lr_cp_store_param_rsi_end:

.globl lr_cp_store_param_rdx_begin
.globl lr_cp_store_param_rdx_end
lr_cp_store_param_rdx_begin:
    mov [rbp + 0x33333333], rdx
lr_cp_store_param_rdx_end:

.globl lr_cp_store_param_rcx_begin
.globl lr_cp_store_param_rcx_end
lr_cp_store_param_rcx_begin:
    mov [rbp + 0x33333333], rcx
lr_cp_store_param_rcx_end:

.globl lr_cp_store_param_r8_begin
.globl lr_cp_store_param_r8_end
lr_cp_store_param_r8_begin:
    mov [rbp + 0x33333333], r8
lr_cp_store_param_r8_end:

.globl lr_cp_store_param_r9_begin
.globl lr_cp_store_param_r9_end
lr_cp_store_param_r9_begin:
    mov [rbp + 0x33333333], r9
lr_cp_store_param_r9_end:

.section .note.GNU-stack,"",@progbits
