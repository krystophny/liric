#include "platform.h"

#include <string.h>

typedef struct {
    const char *name;
    const uint8_t *blob_begin;
    const uint8_t *blob_end;
} lr_platform_intrinsic_desc_t;

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#define LR_PLATFORM_HAS_INTRINSIC_BLOBS 1
extern const uint8_t lr_stub_llvm_fabs_f32_begin[];
extern const uint8_t lr_stub_llvm_fabs_f32_end[];
extern const uint8_t lr_stub_llvm_fabs_f64_begin[];
extern const uint8_t lr_stub_llvm_fabs_f64_end[];
extern const uint8_t lr_stub_llvm_sqrt_f32_begin[];
extern const uint8_t lr_stub_llvm_sqrt_f32_end[];
extern const uint8_t lr_stub_llvm_sqrt_f64_begin[];
extern const uint8_t lr_stub_llvm_sqrt_f64_end[];
extern const uint8_t lr_stub_llvm_exp_f32_begin[];
extern const uint8_t lr_stub_llvm_exp_f32_end[];
extern const uint8_t lr_stub_llvm_exp_f64_begin[];
extern const uint8_t lr_stub_llvm_exp_f64_end[];
extern const uint8_t lr_stub_llvm_pow_f32_begin[];
extern const uint8_t lr_stub_llvm_pow_f32_end[];
extern const uint8_t lr_stub_llvm_pow_f64_begin[];
extern const uint8_t lr_stub_llvm_pow_f64_end[];
extern const uint8_t lr_stub_llvm_copysign_f32_begin[];
extern const uint8_t lr_stub_llvm_copysign_f32_end[];
extern const uint8_t lr_stub_llvm_copysign_f64_begin[];
extern const uint8_t lr_stub_llvm_copysign_f64_end[];
extern const uint8_t lr_stub_llvm_powi_f32_i32_begin[];
extern const uint8_t lr_stub_llvm_powi_f32_i32_end[];
extern const uint8_t lr_stub_llvm_powi_f64_i32_begin[];
extern const uint8_t lr_stub_llvm_powi_f64_i32_end[];
extern const uint8_t lr_stub_llvm_powi_f32_i64_begin[];
extern const uint8_t lr_stub_llvm_powi_f32_i64_end[];
extern const uint8_t lr_stub_llvm_powi_f64_i64_begin[];
extern const uint8_t lr_stub_llvm_powi_f64_i64_end[];
extern const uint8_t lr_stub_llvm_memset_i32_begin[];
extern const uint8_t lr_stub_llvm_memset_i32_end[];
extern const uint8_t lr_stub_llvm_memset_i64_begin[];
extern const uint8_t lr_stub_llvm_memset_i64_end[];
extern const uint8_t lr_stub_llvm_memcpy_i32_begin[];
extern const uint8_t lr_stub_llvm_memcpy_i32_end[];
extern const uint8_t lr_stub_llvm_memcpy_i64_begin[];
extern const uint8_t lr_stub_llvm_memcpy_i64_end[];
extern const uint8_t lr_stub_llvm_memmove_i32_begin[];
extern const uint8_t lr_stub_llvm_memmove_i32_end[];
extern const uint8_t lr_stub_llvm_memmove_i64_begin[];
extern const uint8_t lr_stub_llvm_memmove_i64_end[];
#else
#define LR_PLATFORM_HAS_INTRINSIC_BLOBS 0
#endif

#if defined(__linux__) && defined(__x86_64__)
#define LR_PLATFORM_HAS_X86_64_BLOBS 1
extern const uint8_t lr_stub_llvm_sin_f32_begin[];
extern const uint8_t lr_stub_llvm_sin_f32_end[];
extern const uint8_t lr_stub_llvm_sin_f64_begin[];
extern const uint8_t lr_stub_llvm_sin_f64_end[];
extern const uint8_t lr_stub_llvm_cos_f32_begin[];
extern const uint8_t lr_stub_llvm_cos_f32_end[];
extern const uint8_t lr_stub_llvm_cos_f64_begin[];
extern const uint8_t lr_stub_llvm_cos_f64_end[];
extern const uint8_t lr_stub_llvm_log_f32_begin[];
extern const uint8_t lr_stub_llvm_log_f32_end[];
extern const uint8_t lr_stub_llvm_log_f64_begin[];
extern const uint8_t lr_stub_llvm_log_f64_end[];
extern const uint8_t lr_stub_llvm_log2_f32_begin[];
extern const uint8_t lr_stub_llvm_log2_f32_end[];
extern const uint8_t lr_stub_llvm_log2_f64_begin[];
extern const uint8_t lr_stub_llvm_log2_f64_end[];
extern const uint8_t lr_stub_llvm_log10_f32_begin[];
extern const uint8_t lr_stub_llvm_log10_f32_end[];
extern const uint8_t lr_stub_llvm_log10_f64_begin[];
extern const uint8_t lr_stub_llvm_log10_f64_end[];
extern const uint8_t lr_stub_llvm_exp2_f32_begin[];
extern const uint8_t lr_stub_llvm_exp2_f32_end[];
extern const uint8_t lr_stub_llvm_exp2_f64_begin[];
extern const uint8_t lr_stub_llvm_exp2_f64_end[];
extern const uint8_t lr_stub_llvm_floor_f32_begin[];
extern const uint8_t lr_stub_llvm_floor_f32_end[];
extern const uint8_t lr_stub_llvm_floor_f64_begin[];
extern const uint8_t lr_stub_llvm_floor_f64_end[];
extern const uint8_t lr_stub_llvm_ceil_f32_begin[];
extern const uint8_t lr_stub_llvm_ceil_f32_end[];
extern const uint8_t lr_stub_llvm_ceil_f64_begin[];
extern const uint8_t lr_stub_llvm_ceil_f64_end[];
extern const uint8_t lr_stub_llvm_trunc_f32_begin[];
extern const uint8_t lr_stub_llvm_trunc_f32_end[];
extern const uint8_t lr_stub_llvm_trunc_f64_begin[];
extern const uint8_t lr_stub_llvm_trunc_f64_end[];
extern const uint8_t lr_stub_llvm_round_f32_begin[];
extern const uint8_t lr_stub_llvm_round_f32_end[];
extern const uint8_t lr_stub_llvm_round_f64_begin[];
extern const uint8_t lr_stub_llvm_round_f64_end[];
extern const uint8_t lr_stub_llvm_rint_f32_begin[];
extern const uint8_t lr_stub_llvm_rint_f32_end[];
extern const uint8_t lr_stub_llvm_rint_f64_begin[];
extern const uint8_t lr_stub_llvm_rint_f64_end[];
extern const uint8_t lr_stub_llvm_nearbyint_f32_begin[];
extern const uint8_t lr_stub_llvm_nearbyint_f32_end[];
extern const uint8_t lr_stub_llvm_nearbyint_f64_begin[];
extern const uint8_t lr_stub_llvm_nearbyint_f64_end[];
extern const uint8_t lr_stub_llvm_fma_f32_begin[];
extern const uint8_t lr_stub_llvm_fma_f32_end[];
extern const uint8_t lr_stub_llvm_fma_f64_begin[];
extern const uint8_t lr_stub_llvm_fma_f64_end[];
extern const uint8_t lr_stub_llvm_minnum_f32_begin[];
extern const uint8_t lr_stub_llvm_minnum_f32_end[];
extern const uint8_t lr_stub_llvm_minnum_f64_begin[];
extern const uint8_t lr_stub_llvm_minnum_f64_end[];
extern const uint8_t lr_stub_llvm_maxnum_f32_begin[];
extern const uint8_t lr_stub_llvm_maxnum_f32_end[];
extern const uint8_t lr_stub_llvm_maxnum_f64_begin[];
extern const uint8_t lr_stub_llvm_maxnum_f64_end[];
extern const uint8_t lr_stub_llvm_abs_i32_begin[];
extern const uint8_t lr_stub_llvm_abs_i32_end[];
extern const uint8_t lr_stub_llvm_abs_i64_begin[];
extern const uint8_t lr_stub_llvm_abs_i64_end[];
extern const uint8_t lr_stub_llvm_is_fpclass_f32_begin[];
extern const uint8_t lr_stub_llvm_is_fpclass_f32_end[];
extern const uint8_t lr_stub_llvm_is_fpclass_f64_begin[];
extern const uint8_t lr_stub_llvm_is_fpclass_f64_end[];
#else
#if !defined(LR_PLATFORM_HAS_X86_64_BLOBS)
#define LR_PLATFORM_HAS_X86_64_BLOBS 0
#endif
#endif

#if LR_PLATFORM_HAS_INTRINSIC_BLOBS
#define LR_STUB_BLOB(begin, end) begin, end
#else
#define LR_STUB_BLOB(begin, end) NULL, NULL
#endif

#if LR_PLATFORM_HAS_X86_64_BLOBS
#define LR_X86_BLOB(begin, end) begin, end
#else
#define LR_X86_BLOB(begin, end) NULL, NULL
#endif

static const lr_platform_intrinsic_desc_t g_intrinsics[] = {
    { "llvm.fabs.f32", LR_STUB_BLOB(lr_stub_llvm_fabs_f32_begin, lr_stub_llvm_fabs_f32_end) },
    { "llvm.fabs.f64", LR_STUB_BLOB(lr_stub_llvm_fabs_f64_begin, lr_stub_llvm_fabs_f64_end) },
    { "llvm.sqrt.f32", LR_STUB_BLOB(lr_stub_llvm_sqrt_f32_begin, lr_stub_llvm_sqrt_f32_end) },
    { "llvm.sqrt.f64", LR_STUB_BLOB(lr_stub_llvm_sqrt_f64_begin, lr_stub_llvm_sqrt_f64_end) },
    { "llvm.exp.f32", LR_STUB_BLOB(lr_stub_llvm_exp_f32_begin, lr_stub_llvm_exp_f32_end) },
    { "llvm.exp.f64", LR_STUB_BLOB(lr_stub_llvm_exp_f64_begin, lr_stub_llvm_exp_f64_end) },
    { "llvm.pow.f32", LR_STUB_BLOB(lr_stub_llvm_pow_f32_begin, lr_stub_llvm_pow_f32_end) },
    { "llvm.pow.f64", LR_STUB_BLOB(lr_stub_llvm_pow_f64_begin, lr_stub_llvm_pow_f64_end) },
    { "llvm.copysign.f32", LR_STUB_BLOB(lr_stub_llvm_copysign_f32_begin, lr_stub_llvm_copysign_f32_end) },
    { "llvm.copysign.f64", LR_STUB_BLOB(lr_stub_llvm_copysign_f64_begin, lr_stub_llvm_copysign_f64_end) },
    { "llvm.powi.f32", LR_STUB_BLOB(lr_stub_llvm_powi_f32_i32_begin, lr_stub_llvm_powi_f32_i32_end) },
    { "llvm.powi.f64", LR_STUB_BLOB(lr_stub_llvm_powi_f64_i32_begin, lr_stub_llvm_powi_f64_i32_end) },
    { "llvm.powi.f32.i32", LR_STUB_BLOB(lr_stub_llvm_powi_f32_i32_begin, lr_stub_llvm_powi_f32_i32_end) },
    { "llvm.powi.f64.i32", LR_STUB_BLOB(lr_stub_llvm_powi_f64_i32_begin, lr_stub_llvm_powi_f64_i32_end) },
    { "llvm.powi.f32.i64", LR_STUB_BLOB(lr_stub_llvm_powi_f32_i64_begin, lr_stub_llvm_powi_f32_i64_end) },
    { "llvm.powi.f64.i64", LR_STUB_BLOB(lr_stub_llvm_powi_f64_i64_begin, lr_stub_llvm_powi_f64_i64_end) },
    { "llvm.memset.p0i8.i32", LR_STUB_BLOB(lr_stub_llvm_memset_i32_begin, lr_stub_llvm_memset_i32_end) },
    { "llvm.memset.p0i8.i64", LR_STUB_BLOB(lr_stub_llvm_memset_i64_begin, lr_stub_llvm_memset_i64_end) },
    { "llvm.memset.p0.i32", LR_STUB_BLOB(lr_stub_llvm_memset_i32_begin, lr_stub_llvm_memset_i32_end) },
    { "llvm.memset.p0.i64", LR_STUB_BLOB(lr_stub_llvm_memset_i64_begin, lr_stub_llvm_memset_i64_end) },
    { "llvm.memcpy.p0i8.p0i8.i32", LR_STUB_BLOB(lr_stub_llvm_memcpy_i32_begin, lr_stub_llvm_memcpy_i32_end) },
    { "llvm.memcpy.p0i8.p0i8.i64", LR_STUB_BLOB(lr_stub_llvm_memcpy_i64_begin, lr_stub_llvm_memcpy_i64_end) },
    { "llvm.memcpy.p0.p0.i32", LR_STUB_BLOB(lr_stub_llvm_memcpy_i32_begin, lr_stub_llvm_memcpy_i32_end) },
    { "llvm.memcpy.p0.p0.i64", LR_STUB_BLOB(lr_stub_llvm_memcpy_i64_begin, lr_stub_llvm_memcpy_i64_end) },
    { "llvm.memmove.p0i8.p0i8.i32", LR_STUB_BLOB(lr_stub_llvm_memmove_i32_begin, lr_stub_llvm_memmove_i32_end) },
    { "llvm.memmove.p0i8.p0i8.i64", LR_STUB_BLOB(lr_stub_llvm_memmove_i64_begin, lr_stub_llvm_memmove_i64_end) },
    { "llvm.memmove.p0.p0.i32", LR_STUB_BLOB(lr_stub_llvm_memmove_i32_begin, lr_stub_llvm_memmove_i32_end) },
    { "llvm.memmove.p0.p0.i64", LR_STUB_BLOB(lr_stub_llvm_memmove_i64_begin, lr_stub_llvm_memmove_i64_end) },
    { "llvm.sin.f32", LR_X86_BLOB(lr_stub_llvm_sin_f32_begin, lr_stub_llvm_sin_f32_end) },
    { "llvm.sin.f64", LR_X86_BLOB(lr_stub_llvm_sin_f64_begin, lr_stub_llvm_sin_f64_end) },
    { "llvm.cos.f32", LR_X86_BLOB(lr_stub_llvm_cos_f32_begin, lr_stub_llvm_cos_f32_end) },
    { "llvm.cos.f64", LR_X86_BLOB(lr_stub_llvm_cos_f64_begin, lr_stub_llvm_cos_f64_end) },
    { "llvm.log.f32", LR_X86_BLOB(lr_stub_llvm_log_f32_begin, lr_stub_llvm_log_f32_end) },
    { "llvm.log.f64", LR_X86_BLOB(lr_stub_llvm_log_f64_begin, lr_stub_llvm_log_f64_end) },
    { "llvm.log2.f32", LR_X86_BLOB(lr_stub_llvm_log2_f32_begin, lr_stub_llvm_log2_f32_end) },
    { "llvm.log2.f64", LR_X86_BLOB(lr_stub_llvm_log2_f64_begin, lr_stub_llvm_log2_f64_end) },
    { "llvm.log10.f32", LR_X86_BLOB(lr_stub_llvm_log10_f32_begin, lr_stub_llvm_log10_f32_end) },
    { "llvm.log10.f64", LR_X86_BLOB(lr_stub_llvm_log10_f64_begin, lr_stub_llvm_log10_f64_end) },
    { "llvm.exp2.f32", LR_X86_BLOB(lr_stub_llvm_exp2_f32_begin, lr_stub_llvm_exp2_f32_end) },
    { "llvm.exp2.f64", LR_X86_BLOB(lr_stub_llvm_exp2_f64_begin, lr_stub_llvm_exp2_f64_end) },
    { "llvm.floor.f32", LR_X86_BLOB(lr_stub_llvm_floor_f32_begin, lr_stub_llvm_floor_f32_end) },
    { "llvm.floor.f64", LR_X86_BLOB(lr_stub_llvm_floor_f64_begin, lr_stub_llvm_floor_f64_end) },
    { "llvm.ceil.f32", LR_X86_BLOB(lr_stub_llvm_ceil_f32_begin, lr_stub_llvm_ceil_f32_end) },
    { "llvm.ceil.f64", LR_X86_BLOB(lr_stub_llvm_ceil_f64_begin, lr_stub_llvm_ceil_f64_end) },
    { "llvm.trunc.f32", LR_X86_BLOB(lr_stub_llvm_trunc_f32_begin, lr_stub_llvm_trunc_f32_end) },
    { "llvm.trunc.f64", LR_X86_BLOB(lr_stub_llvm_trunc_f64_begin, lr_stub_llvm_trunc_f64_end) },
    { "llvm.round.f32", LR_X86_BLOB(lr_stub_llvm_round_f32_begin, lr_stub_llvm_round_f32_end) },
    { "llvm.round.f64", LR_X86_BLOB(lr_stub_llvm_round_f64_begin, lr_stub_llvm_round_f64_end) },
    { "llvm.rint.f32", LR_X86_BLOB(lr_stub_llvm_rint_f32_begin, lr_stub_llvm_rint_f32_end) },
    { "llvm.rint.f64", LR_X86_BLOB(lr_stub_llvm_rint_f64_begin, lr_stub_llvm_rint_f64_end) },
    { "llvm.nearbyint.f32", LR_X86_BLOB(lr_stub_llvm_nearbyint_f32_begin, lr_stub_llvm_nearbyint_f32_end) },
    { "llvm.nearbyint.f64", LR_X86_BLOB(lr_stub_llvm_nearbyint_f64_begin, lr_stub_llvm_nearbyint_f64_end) },
    { "llvm.fma.f32", LR_X86_BLOB(lr_stub_llvm_fma_f32_begin, lr_stub_llvm_fma_f32_end) },
    { "llvm.fma.f64", LR_X86_BLOB(lr_stub_llvm_fma_f64_begin, lr_stub_llvm_fma_f64_end) },
    { "llvm.fmuladd.f32", LR_X86_BLOB(lr_stub_llvm_fma_f32_begin, lr_stub_llvm_fma_f32_end) },
    { "llvm.fmuladd.f64", LR_X86_BLOB(lr_stub_llvm_fma_f64_begin, lr_stub_llvm_fma_f64_end) },
    { "llvm.minnum.f32", LR_X86_BLOB(lr_stub_llvm_minnum_f32_begin, lr_stub_llvm_minnum_f32_end) },
    { "llvm.minnum.f64", LR_X86_BLOB(lr_stub_llvm_minnum_f64_begin, lr_stub_llvm_minnum_f64_end) },
    { "llvm.maxnum.f32", LR_X86_BLOB(lr_stub_llvm_maxnum_f32_begin, lr_stub_llvm_maxnum_f32_end) },
    { "llvm.maxnum.f64", LR_X86_BLOB(lr_stub_llvm_maxnum_f64_begin, lr_stub_llvm_maxnum_f64_end) },
    { "llvm.abs.i32", LR_X86_BLOB(lr_stub_llvm_abs_i32_begin, lr_stub_llvm_abs_i32_end) },
    { "llvm.abs.i64", LR_X86_BLOB(lr_stub_llvm_abs_i64_begin, lr_stub_llvm_abs_i64_end) },
    { "llvm.is.fpclass.f32", LR_X86_BLOB(lr_stub_llvm_is_fpclass_f32_begin, lr_stub_llvm_is_fpclass_f32_end) },
    { "llvm.is.fpclass.f64", LR_X86_BLOB(lr_stub_llvm_is_fpclass_f64_begin, lr_stub_llvm_is_fpclass_f64_end) },
};

static const lr_platform_intrinsic_desc_t *lookup_intrinsic(const char *name) {
    if (!name || !name[0])
        return NULL;

    size_t n = sizeof(g_intrinsics) / sizeof(g_intrinsics[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(g_intrinsics[i].name, name) == 0)
            return &g_intrinsics[i];
    }
    return NULL;
}

bool lr_platform_intrinsic_supported(const char *name) {
    const lr_platform_intrinsic_desc_t *d = lookup_intrinsic(name);
    return d && d->blob_begin && d->blob_end && d->blob_end > d->blob_begin;
}

bool lr_platform_intrinsic_blob_lookup(const char *name,
                                       const uint8_t **begin,
                                       const uint8_t **end) {
    const lr_platform_intrinsic_desc_t *d = lookup_intrinsic(name);
    if (!d || !begin || !end || !d->blob_begin || !d->blob_end || d->blob_end <= d->blob_begin)
        return false;
    *begin = d->blob_begin;
    *end = d->blob_end;
    return true;
}

size_t lr_platform_intrinsic_count(void) {
    return sizeof(g_intrinsics) / sizeof(g_intrinsics[0]);
}

const char *lr_platform_intrinsic_name(size_t idx) {
    size_t n = sizeof(g_intrinsics) / sizeof(g_intrinsics[0]);
    if (idx >= n)
        return NULL;
    return g_intrinsics[idx].name;
}
